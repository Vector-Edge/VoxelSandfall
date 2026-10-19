#include "Physics.h"

#include <algorithm>

namespace {

constexpr float kSphereR = 0.5f;           // collision radius of one voxel
constexpr float kMaxSpeed = 420.0f;        // keeps per-step travel below half the 3-voxel wall thickness
constexpr float kMaxAngSpeed = 20.0f;
constexpr int kSolverIterations = 12;
constexpr float kFriction = 0.7f;

void labelComponents(const std::vector<IVec3>& cells, std::vector<std::vector<IVec3>>& out) {
    IVec3 mn = cells[0], mx = cells[0];
    for (const IVec3& c : cells) {
        mn = IVec3(std::min(mn.x, c.x), std::min(mn.y, c.y), std::min(mn.z, c.z));
        mx = IVec3(std::max(mx.x, c.x), std::max(mx.y, c.y), std::max(mx.z, c.z));
    }
    const size_t dx = (size_t)(mx.x - mn.x + 1), dy = (size_t)(mx.y - mn.y + 1), dz = (size_t)(mx.z - mn.z + 1);
    std::vector<int> grid(dx * dy * dz, -1);
    auto gi = [&](const IVec3& c) {
        return (size_t)(c.x - mn.x) + dx * ((size_t)(c.y - mn.y) + dy * (size_t)(c.z - mn.z));
    };
    for (size_t i = 0; i < cells.size(); i++) grid[gi(cells[i])] = (int)i;
    std::vector<uint8_t> seen(cells.size(), 0);
    for (size_t i = 0; i < cells.size(); i++)
        if (grid[gi(cells[i])] != (int)i) seen[i] = 1;  // duplicate coordinate: ignore
    std::vector<int> queue;
    static const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (size_t s = 0; s < cells.size(); s++) {
        if (seen[s]) continue;
        queue.clear();
        queue.push_back((int)s);
        seen[s] = 1;
        for (size_t h = 0; h < queue.size(); h++) {
            const IVec3 c = cells[queue[h]];
            for (const auto& d : nb) {
                IVec3 n(c.x + d[0], c.y + d[1], c.z + d[2]);
                if (n.x < mn.x || n.y < mn.y || n.z < mn.z || n.x > mx.x || n.y > mx.y || n.z > mx.z) continue;
                int j = grid[gi(n)];
                if (j >= 0 && !seen[j]) {
                    seen[j] = 1;
                    queue.push_back(j);
                }
            }
        }
        out.emplace_back();
        out.back().reserve(queue.size());
        for (int q : queue) out.back().push_back(cells[q]);
    }
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Body
// ------------------------------------------------------------------------------------------------
void Body::assignCells(const std::vector<IVec3>& cells) {
    IVec3 mn = cells[0], mx = cells[0];
    for (const IVec3& c : cells) {
        mn = IVec3(std::min(mn.x, c.x), std::min(mn.y, c.y), std::min(mn.z, c.z));
        mx = IVec3(std::max(mx.x, c.x), std::max(mx.y, c.y), std::max(mx.z, c.z));
    }
    const Vec3 oldCom = com;
    nx = mx.x - mn.x + 1;
    ny = mx.y - mn.y + 1;
    nz = mx.z - mn.z + 1;
    occ.assign((size_t)nx * ny * nz, 0);
    double sx = 0, sy = 0, sz = 0;
    for (const IVec3& c : cells) {
        int i = c.x - mn.x, j = c.y - mn.y, k = c.z - mn.z;
        occ[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)] = 1;
        sx += i + 0.5;
        sy += j + 0.5;
        sz += k + 0.5;
    }
    const double inv = 1.0 / (double)cells.size();
    const Vec3 newCom((float)(sx * inv), (float)(sy * inv), (float)(sz * inv));
    // World placement of the new COM, expressed through the old lattice transform.
    const Vec3 r = rot.rotate((newCom + Vec3((float)mn.x, (float)mn.y, (float)mn.z)) - oldCom);
    pos = pos + r;
    vel = vel + cross(ang, r);
    com = newCom;
    computeMassProps();
    version++;
    warm.clear();
    warmPrev.clear();
    ovDirty = true;
}

void Body::collectCells(std::vector<IVec3>& out) const {
    out.clear();
    out.reserve((size_t)count);
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++)
                if (occ[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)]) out.emplace_back(i, j, k);
}

void Body::computeMassProps() {
    surf.clear();
    count = 0;
    double Ixx = 0, Iyy = 0, Izz = 0, Ixy = 0, Ixz = 0, Iyz = 0;
    float maxR2 = 0;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                if (!occupiedAt(i, j, k)) continue;
                count++;
                const Vec3 r(i + 0.5f - com.x, j + 0.5f - com.y, k + 0.5f - com.z);
                Ixx += (double)r.y * r.y + (double)r.z * r.z;
                Iyy += (double)r.x * r.x + (double)r.z * r.z;
                Izz += (double)r.x * r.x + (double)r.y * r.y;
                Ixy -= (double)r.x * r.y;
                Ixz -= (double)r.x * r.z;
                Iyz -= (double)r.y * r.z;
                if (!occupiedAt(i + 1, j, k) || !occupiedAt(i - 1, j, k) || !occupiedAt(i, j + 1, k) ||
                    !occupiedAt(i, j - 1, k) || !occupiedAt(i, j, k + 1) || !occupiedAt(i, j, k - 1)) {
                    surf.push_back(r);
                    maxR2 = std::max(maxR2, lengthSq(r));
                }
            }
    const double cube = (double)count / 6.0;  // inertia of each unit cube about its own centre
    Ixx += cube;
    Iyy += cube;
    Izz += cube;
    mass = (float)count;
    invMass = count > 0 ? 1.0f / mass : 0.0f;
    Mat3 I;
    I.m[0][0] = (float)Ixx; I.m[0][1] = (float)Ixy; I.m[0][2] = (float)Ixz;
    I.m[1][0] = (float)Ixy; I.m[1][1] = (float)Iyy; I.m[1][2] = (float)Iyz;
    I.m[2][0] = (float)Ixz; I.m[2][1] = (float)Iyz; I.m[2][2] = (float)Izz;
    invIbody = I.inverse();
    radius = std::sqrt(maxR2) + 0.9f;
    updateInertiaWorld();
}

void Body::updateInertiaWorld() {
    Mat3 R = rot.toMat3();
    invIworld = R * invIbody * R.transposed();
}

// ------------------------------------------------------------------------------------------------
// Physics: creation / editing
// ------------------------------------------------------------------------------------------------
Body* Physics::createFromWorldCells(const std::vector<IVec3>& cells) {
    auto up = std::make_unique<Body>();
    up->id = nextId++;
    up->assignCells(cells);
    Body* p = up.get();
    bodies.push_back(std::move(up));
    return p;
}

void Physics::setBodyCells(Body& b, const std::vector<IVec3>& cells) {
    if (cells.empty()) {
        b.dead = true;
        return;
    }
    std::vector<std::vector<IVec3>> comps;
    labelComponents(cells, comps);
    size_t largest = 0;
    for (size_t i = 1; i < comps.size(); i++)
        if (comps[i].size() > comps[largest].size()) largest = i;
    // Every detached piece inherits the parent's motion (v + w x r is applied inside assignCells).
    for (size_t i = 0; i < comps.size(); i++) {
        if (i == largest) continue;
        auto nb = std::make_unique<Body>();
        nb->id = nextId++;
        nb->pos = b.pos;
        nb->com = b.com;
        nb->rot = b.rot;
        nb->vel = b.vel;
        nb->ang = b.ang;
        nb->assignCells(comps[i]);
        bodies.push_back(std::move(nb));
    }
    b.assignCells(comps[largest]);
    b.wake();
}

void Physics::wakeNear(const Vec3& c, float r) {
    for (auto& up : bodies)
        if (length(up->pos - c) < up->radius + r) up->wake();
}

void Physics::wakeAll() {
    for (auto& up : bodies) up->wake();
}

size_t Physics::awakeCount() const {
    size_t n = 0;
    for (auto& up : bodies) n += up->asleep ? 0 : 1;
    return n;
}

void Physics::clearOverlay(Body& b) {
    for (uint32_t i : b.ovCells)
        if (world.overlay[i]) world.overlay[i]--;
    b.ovCells.clear();
}

void Physics::purgeDead() {
    bool any = false;
    for (auto& up : bodies)
        if (up->dead) {
            clearOverlay(*up);
            any = true;
        }
    if (any) bodies.erase(std::remove_if(bodies.begin(), bodies.end(), [](const std::unique_ptr<Body>& b) { return b->dead; }), bodies.end());
}

void Physics::rasterizeOverlay() {
    purgeDead();
    World& w = world;
    auto mark = [&](Body& b, int x, int y, int z) {
        if (!w.inBounds(x, y, z)) return;
        size_t i = w.idx(x, y, z);
        if (w.overlay[i] < 255) {
            w.overlay[i]++;
            b.ovCells.push_back((uint32_t)i);
        }
    };
    for (auto& up : bodies) {
        Body& b = *up;
        if (b.asleep && !b.ovDirty) continue;
        clearOverlay(b);
        const Mat3 R = b.rot.toMat3();
        for (int k = 0; k < b.nz; k++)
            for (int j = 0; j < b.ny; j++)
                for (int i = 0; i < b.nx; i++) {
                    if (!b.occupiedAt(i, j, k)) continue;
                    Vec3 p = b.pos + R * Vec3(i + 0.5f - b.com.x, j + 0.5f - b.com.y, k + 0.5f - b.com.z);
                    mark(b, floorToInt(p.x), floorToInt(p.y), floorToInt(p.z));
                }
        for (const Vec3& s : b.surf) {  // dilate the surface so rotated bodies leave no gaps
            Vec3 p = b.pos + R * s;
            int x0 = floorToInt(p.x - 0.45f), x1 = floorToInt(p.x + 0.45f);
            int y0 = floorToInt(p.y - 0.45f), y1 = floorToInt(p.y + 0.45f);
            int z0 = floorToInt(p.z - 0.45f), z1 = floorToInt(p.z + 0.45f);
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) mark(b, x, y, z);
        }
        b.ovDirty = false;
    }
    bool any = false;
    for (auto& up : bodies)
        if (!up->ovCells.empty()) { any = true; break; }
    world.overlayActive = any;
}

// ------------------------------------------------------------------------------------------------
// Collision detection
// ------------------------------------------------------------------------------------------------
void Physics::collideWorld(Body& a, std::vector<Contact>& out) {
    const Mat3 R = a.rot.toMat3();
    a.warmPrev.swap(a.warm);
    a.warm.assign(a.surf.size(), Vec3(0, 0, 0));
    const bool haveWarm = a.warmPrev.size() == a.surf.size();
    const World& w = world;

    for (size_t si = 0; si < a.surf.size(); si++) {
        const Vec3 p = a.pos + R * a.surf[si];
        const int x0 = floorToInt(p.x - kSphereR), x1 = floorToInt(p.x + kSphereR);
        const int y0 = floorToInt(p.y - kSphereR), y1 = floorToInt(p.y + kSphereR);
        const int z0 = floorToInt(p.z - kSphereR), z1 = floorToInt(p.z + kSphereR);
        float bestDepth = 0;
        Vec3 bestN, bestP;
        for (int z = z0; z <= z1; z++)
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    if (!w.rigidSolid(x, y, z)) continue;
                    const Vec3 q(clampf(p.x, (float)x, x + 1.0f), clampf(p.y, (float)y, y + 1.0f),
                                 clampf(p.z, (float)z, z + 1.0f));
                    const Vec3 d = p - q;
                    const float d2 = lengthSq(d);
                    if (d2 >= kSphereR * kSphereR) continue;
                    Vec3 n, cp;
                    float depth;
                    if (d2 > 1e-10f) {
                        const float dist = std::sqrt(d2);
                        n = d / dist;
                        depth = kSphereR - dist;
                        cp = q;
                        // Reject "ghost" contacts against faces that are buried inside the solid.
                        int ax = 0;
                        float av = std::fabs(n.x);
                        if (std::fabs(n.y) > av) { ax = 1; av = std::fabs(n.y); }
                        if (std::fabs(n.z) > av) { ax = 2; }
                        int cell[3] = {x, y, z};
                        cell[ax] += n[ax] > 0 ? 1 : -1;
                        if (w.rigidSolid(cell[0], cell[1], cell[2])) continue;
                    } else {
                        // Sphere centre inside the voxel: leave through the nearest exposed face.
                        const int cm[3] = {x, y, z};
                        float bd = 1e9f;
                        int bax = -1, bsg = 0;
                        for (int ax = 0; ax < 3; ax++) {
                            int lo[3] = {x, y, z}, hi[3] = {x, y, z};
                            lo[ax]--;
                            hi[ax]++;
                            float dlo = p[ax] - (float)cm[ax], dhi = (float)(cm[ax] + 1) - p[ax];
                            if (dlo < bd && !w.rigidSolid(lo[0], lo[1], lo[2])) { bd = dlo; bax = ax; bsg = -1; }
                            if (dhi < bd && !w.rigidSolid(hi[0], hi[1], hi[2])) { bd = dhi; bax = ax; bsg = 1; }
                        }
                        if (bax < 0) continue;
                        n = Vec3(0, 0, 0);
                        n[bax] = (float)bsg;
                        depth = kSphereR + bd;
                        cp = p + n * bd;
                    }
                    if (depth > bestDepth) {
                        bestDepth = depth;
                        bestN = n;
                        bestP = cp;
                    }
                }
        if (bestDepth > 0) {
            Contact c;
            c.a = &a;
            c.n = bestN;
            c.depth = bestDepth;
            c.ra = bestP - a.pos;
            c.si = (int)si;
            a.touching = true;
            if (haveWarm) {
                const Vec3& wv = a.warmPrev[si];
                c.jn = wv.x;
                c.jt1 = wv.y;
                c.jt2 = wv.z;
            }
            out.push_back(c);
        }
    }
}

void Physics::collidePair(Body& a, Body& b, std::vector<Contact>& out) {
    const Mat3 Ra = a.rot.toMat3();
    const Mat3 Rb = b.rot.toMat3();
    const Mat3 RbT = Rb.transposed();
    for (const Vec3& s : a.surf) {
        const Vec3 p = a.pos + Ra * s;
        const Vec3 q = RbT * (p - b.pos) + b.com;  // sphere centre in b's lattice
        const int ci = floorToInt(q.x), cj = floorToInt(q.y), ck = floorToInt(q.z);
        if (ci < -1 || cj < -1 || ck < -1 || ci > b.nx || cj > b.ny || ck > b.nz) continue;
        for (int k = ck - 1; k <= ck + 1; k++)
            for (int j = cj - 1; j <= cj + 1; j++)
                for (int i = ci - 1; i <= ci + 1; i++) {
                    if (!b.occupiedAt(i, j, k)) continue;
                    const Vec3 d = q - Vec3(i + 0.5f, j + 0.5f, k + 0.5f);
                    const float d2 = lengthSq(d);
                    if (d2 >= 1.0f) continue;
                    const float dist = std::sqrt(d2);
                    const Vec3 nl = dist > 1e-5f ? d / dist : Vec3(0, 1, 0);
                    a.touching = b.touching = true;
                    Contact c;
                    c.a = &a;
                    c.b = &b;
                    c.n = Rb * nl;
                    c.depth = 1.0f - dist;
                    const Vec3 cp = p - c.n * 0.5f;
                    c.ra = cp - a.pos;
                    c.rb = cp - b.pos;
                    out.push_back(c);
                }
    }
}

// ------------------------------------------------------------------------------------------------
// Solver
// ------------------------------------------------------------------------------------------------
void Physics::solve(std::vector<Contact>& cs, float dt) {
    const float beta = 0.2f, slop = 0.02f, maxBias = 30.0f;
    auto apply = [](Body* body, const Vec3& P, const Vec3& r) {
        body->vel += P * body->invMass;
        body->ang += body->invIworld * cross(r, P);
    };

    for (Contact& c : cs) {
        Body* a = c.a;
        Body* b = (c.b && !c.b->asleep) ? c.b : nullptr;  // sleeping bodies act as static geometry
        const Vec3 ref = std::fabs(c.n.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        c.t1 = normalize(cross(c.n, ref));
        c.t2 = cross(c.n, c.t1);
        auto eff = [&](const Vec3& dir) {
            float k = a->invMass + dot(cross(a->invIworld * cross(c.ra, dir), c.ra), dir);
            if (b) k += b->invMass + dot(cross(b->invIworld * cross(c.rb, dir), c.rb), dir);
            return k > 0 ? 1.0f / k : 0.0f;
        };
        c.mn = eff(c.n);
        c.mt1 = eff(c.t1);
        c.mt2 = eff(c.t2);
        c.bias = std::min(std::max(0.0f, c.depth - slop) * beta / dt, maxBias);
        if (c.jn != 0 || c.jt1 != 0 || c.jt2 != 0) {  // warm start
            const Vec3 P = c.n * c.jn + c.t1 * c.jt1 + c.t2 * c.jt2;
            apply(a, P, c.ra);
            if (b) apply(b, -P, c.rb);
        }
    }

    for (int it = 0; it < kSolverIterations; it++) {
        for (Contact& c : cs) {
            Body* a = c.a;
            Body* b = (c.b && !c.b->asleep) ? c.b : nullptr;
            auto relVel = [&]() {
                Vec3 va = a->vel + cross(a->ang, c.ra);
                Vec3 vb = b ? b->vel + cross(b->ang, c.rb) : Vec3(0, 0, 0);
                return va - vb;
            };
            // Normal.
            {
                float vn = dot(relVel(), c.n);
                float lam = (c.bias - vn) * c.mn;
                float nj = std::max(c.jn + lam, 0.0f);
                lam = nj - c.jn;
                c.jn = nj;
                const Vec3 P = c.n * lam;
                apply(a, P, c.ra);
                if (b) apply(b, -P, c.rb);
            }
            // Friction (Coulomb, box approximation).
            const float maxF = kFriction * c.jn;
            {
                float vt = dot(relVel(), c.t1);
                float lam = -vt * c.mt1;
                float nj = clampf(c.jt1 + lam, -maxF, maxF);
                lam = nj - c.jt1;
                c.jt1 = nj;
                const Vec3 P = c.t1 * lam;
                apply(a, P, c.ra);
                if (b) apply(b, -P, c.rb);
            }
            {
                float vt = dot(relVel(), c.t2);
                float lam = -vt * c.mt2;
                float nj = clampf(c.jt2 + lam, -maxF, maxF);
                lam = nj - c.jt2;
                c.jt2 = nj;
                const Vec3 P = c.t2 * lam;
                apply(a, P, c.ra);
                if (b) apply(b, -P, c.rb);
            }
        }
    }

    for (Contact& c : cs)
        if (!c.b && c.si >= 0 && (size_t)c.si < c.a->warm.size()) c.a->warm[c.si] = Vec3(c.jn, c.jt1, c.jt2);
}

// ------------------------------------------------------------------------------------------------
// Step
// ------------------------------------------------------------------------------------------------
void Physics::step(float dt) {
    purgeDead();
    contacts_.clear();

    for (auto& up : bodies) {
        Body& b = *up;
        if (b.asleep) continue;
        b.vel.y -= cfg::GRAVITY * dt;
        b.touching = false;
        float sp = length(b.vel);
        if (sp > kMaxSpeed) b.vel *= kMaxSpeed / sp;
        float av = length(b.ang);
        if (av > kMaxAngSpeed) b.ang *= kMaxAngSpeed / av;
    }

    // World contacts.
    for (auto& up : bodies)
        if (!up->asleep) collideWorld(*up, contacts_);

    // Body vs body: sweep along X, then sphere-vs-sphere on the voxel lattices.
    sortedBodies_.clear();
    for (auto& up : bodies) sortedBodies_.push_back(up.get());
    std::sort(sortedBodies_.begin(), sortedBodies_.end(),
              [](const Body* l, const Body* r) { return l->pos.x - l->radius < r->pos.x - r->radius; });
    for (size_t i = 0; i < sortedBodies_.size(); i++) {
        Body* A = sortedBodies_[i];
        for (size_t j = i + 1; j < sortedBodies_.size(); j++) {
            Body* B = sortedBodies_[j];
            if (B->pos.x - B->radius > A->pos.x + A->radius) break;
            if (A->asleep && B->asleep) continue;
            if (std::fabs(A->pos.y - B->pos.y) > A->radius + B->radius) continue;
            if (std::fabs(A->pos.z - B->pos.z) > A->radius + B->radius) continue;
            if (lengthSq(A->pos - B->pos) > (A->radius + B->radius) * (A->radius + B->radius)) continue;
            // The awake body with fewer surface voxels scans the other body's lattice.
            Body* it = A;
            Body* tg = B;
            if (A->asleep || (!B->asleep && B->surf.size() < A->surf.size())) std::swap(it, tg);
            if (tg->asleep && lengthSq(it->vel) > 25.0f * 25.0f) tg->wake();
            collidePair(*it, *tg, contacts_);
        }
    }

    lastContactCount = (int)contacts_.size();
    solve(contacts_, dt);

    for (auto& up : bodies) {
        Body& b = *up;
        if (b.asleep) continue;
        b.pos += b.vel * dt;
        b.rot = b.rot.integrate(b.ang, dt);
        // Air drag is tiny; touching bodies get rolling resistance so rubble comes to rest.
        b.ang *= 1.0f / (1.0f + (b.touching ? 3.5f : 0.3f) * dt);
        if (b.touching) b.vel *= 1.0f / (1.0f + 0.35f * dt);
        b.updateInertiaWorld();

        const float lin2 = lengthSq(b.vel);
        const float ang2 = lengthSq(b.ang) * b.radius * b.radius;
        if (lin2 < 6.0f * 6.0f && ang2 < 6.0f * 6.0f)
            b.sleepTimer += dt;
        else
            b.sleepTimer = 0;
        if (b.sleepTimer > 0.5f) {
            b.asleep = true;
            b.vel = Vec3(0, 0, 0);
            b.ang = Vec3(0, 0, 0);
            b.ovDirty = true;
        }
        if (b.pos.y < -600.0f) b.dead = true;
    }
}
