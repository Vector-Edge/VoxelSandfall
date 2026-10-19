#include "Tools.h"

#include <algorithm>
#include <unordered_map>

const char* materialName(uint8_t m) {
    switch (m) {
        case M_SAND: return "SAND";
        case M_WATER: return "WATER";
        case M_STONE: return "NORMAL STONE";
        case M_BEDROCK: return "PERMANENT STONE";
        default: return "AIR";
    }
}

namespace {
// Accumulates the inclusive bounds of bulk-edited cells so World::notifyBox runs once per operation.
struct EditBox {
    int x0 = 1 << 30, y0 = 1 << 30, z0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30), z1 = -(1 << 30);
    void add(int x, int y, int z) {
        x0 = std::min(x0, x); y0 = std::min(y0, y); z0 = std::min(z0, z);
        x1 = std::max(x1, x); y1 = std::max(y1, y); z1 = std::max(z1, z);
    }
    bool valid() const { return x1 >= x0; }
    void flush(World& w) const {
        if (valid()) w.notifyBox(x0, y0, z0, x1, y1, z1);
    }
};
}  // namespace

// ------------------------------------------------------------------------------------------------
// Ray casting against rigid bodies (DDA through the body's lattice)
// ------------------------------------------------------------------------------------------------
static bool rayBody(const Body& b, const Vec3& o, const Vec3& d, float maxDist, float& tOut, Vec3& nOut, IVec3& cellOut) {
    const Vec3 oc = b.pos - o;
    const float tc = dot(oc, d);
    if (tc < -b.radius || tc > maxDist + b.radius) return false;
    if (lengthSq(oc) - tc * tc > b.radius * b.radius) return false;

    const Quat inv = b.rot.conjugate();
    const Vec3 ol = inv.rotate(o - b.pos) + b.com;
    const Vec3 dl = inv.rotate(d);
    const float dims[3] = {(float)b.nx, (float)b.ny, (float)b.nz};
    float t0 = 0, t1 = maxDist;
    for (int a = 0; a < 3; a++) {
        if (std::fabs(dl[a]) < 1e-9f) {
            if (ol[a] < 0 || ol[a] >= dims[a]) return false;
        } else {
            float tn = (0 - ol[a]) / dl[a], tf = (dims[a] - ol[a]) / dl[a];
            if (tn > tf) std::swap(tn, tf);
            t0 = std::max(t0, tn);
            t1 = std::min(t1, tf);
            if (t0 > t1) return false;
        }
    }
    const Vec3 p = ol + dl * (t0 + 1e-4f);
    int cell[3] = {std::clamp(floorToInt(p.x), 0, b.nx - 1), std::clamp(floorToInt(p.y), 0, b.ny - 1),
                   std::clamp(floorToInt(p.z), 0, b.nz - 1)};
    int step[3];
    float tMax[3], tDelta[3];
    for (int a = 0; a < 3; a++) {
        if (dl[a] > 0) {
            step[a] = 1;
            tDelta[a] = 1.0f / dl[a];
            tMax[a] = t0 + ((cell[a] + 1) - p[a]) * tDelta[a];
        } else if (dl[a] < 0) {
            step[a] = -1;
            tDelta[a] = -1.0f / dl[a];
            tMax[a] = t0 + (p[a] - cell[a]) * tDelta[a];
        } else {
            step[a] = 0;
            tDelta[a] = 1e30f;
            tMax[a] = 1e30f;
        }
    }
    int normal[3] = {0, 0, 0};
    {
        int ax = 0;
        float best = 0;
        for (int a = 0; a < 3; a++)
            if (std::fabs(dl[a]) > best) { best = std::fabs(dl[a]); ax = a; }
        normal[ax] = dl[ax] > 0 ? -1 : 1;
    }
    float tEnter = t0;
    const int maxSteps = b.nx + b.ny + b.nz + 4;
    for (int s = 0; s < maxSteps; s++) {
        if (b.occupiedAt(cell[0], cell[1], cell[2])) {
            tOut = tEnter;
            nOut = b.rot.rotate(Vec3((float)normal[0], (float)normal[1], (float)normal[2]));
            cellOut = IVec3(cell[0], cell[1], cell[2]);
            return true;
        }
        int a = 0;
        if (tMax[1] < tMax[a]) a = 1;
        if (tMax[2] < tMax[a]) a = 2;
        if (tMax[a] > t1) return false;
        tEnter = tMax[a];
        cell[a] += step[a];
        tMax[a] += tDelta[a];
        normal[0] = normal[1] = normal[2] = 0;
        normal[a] = -step[a];
        if (cell[a] < 0 || cell[a] >= (a == 0 ? b.nx : a == 1 ? b.ny : b.nz)) return false;
    }
    return false;
}

SceneHit castScene(const World& w, const Physics& ph, const Vec3& origin, const Vec3& dir, float maxDist, uint32_t mask) {
    SceneHit best;
    RayHit rh = w.raycast(origin, dir, maxDist, mask);
    if (rh.hit) {
        best.hit = true;
        best.t = rh.t;
        best.point = rh.point;
        best.normal = Vec3((float)rh.normal.x, (float)rh.normal.y, (float)rh.normal.z);
        best.cell = rh.cell;
        best.material = w.get(rh.cell.x, rh.cell.y, rh.cell.z);
    }
    for (auto& up : ph.bodies) {
        const Body& b = *up;
        if (b.dead) continue;
        float t;
        Vec3 n;
        IVec3 c;
        if (!rayBody(b, origin, dir, best.hit ? best.t : maxDist, t, n, c)) continue;
        if (best.hit && t >= best.t) continue;
        best.hit = true;
        best.t = t;
        best.point = origin + dir * t;
        best.normal = n;
        best.body = const_cast<Body*>(&b);
        best.cell = c;
        best.material = M_STONE;
    }
    return best;
}

// ------------------------------------------------------------------------------------------------
// Island detection: 3D flood fill (BFS) from the cells that touch a carved region
// ------------------------------------------------------------------------------------------------
int detachIslands(World& w, Physics& ph, const std::vector<IVec3>& seeds, int* voxelsMoved) {
    static std::vector<uint64_t> anch, vis;  // anchored-known / visited-in-this-search bitsets
    const size_t total = (size_t)w.chunkCount() << 15;
    if (anch.size() != (total + 63) / 64) {
        anch.assign((total + 63) / 64, 0);
        vis.assign((total + 63) / 64, 0);
    }
    auto testb = [](const std::vector<uint64_t>& v, size_t i) { return ((v[i >> 6] >> (i & 63)) & 1ull) != 0; };
    auto setb = [](std::vector<uint64_t>& v, size_t i) { v[i >> 6] |= 1ull << (i & 63); };
    auto clrb = [](std::vector<uint64_t>& v, size_t i) { v[i >> 6] &= ~(1ull << (i & 63)); };

    std::vector<size_t> anchList;
    std::vector<IVec3> queue;
    int created = 0;
    static const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

    for (const IVec3& s : seeds) {
        if (w.get(s.x, s.y, s.z) != M_STONE) continue;
        const size_t si = w.idx(s.x, s.y, s.z);
        if (testb(anch, si)) continue;

        // Fast path: an intact column of stone down to the bedrock / ground.
        bool anchored = false;
        for (int y = s.y; y >= 0; --y) {
            uint8_t m = w.get(s.x, y, s.z);
            if (m == M_BEDROCK) { anchored = true; break; }
            if (m != M_STONE) break;
            if (y == 0) { anchored = true; break; }
        }
        if (anchored) {
            setb(anch, si);
            anchList.push_back(si);
            continue;
        }

        queue.clear();
        queue.push_back(s);
        setb(vis, si);
        for (size_t h = 0; h < queue.size() && !anchored; h++) {
            const IVec3 c = queue[h];
            if (c.y == 0) { anchored = true; break; }
            for (const auto& d : nb) {
                int nx = c.x + d[0], ny = c.y + d[1], nz = c.z + d[2];
                if (!w.inBounds(nx, ny, nz)) continue;
                size_t ni = w.idx(nx, ny, nz);
                uint8_t m = w.data[ni] & VOX_MASK;
                if (m == M_BEDROCK) { anchored = true; break; }
                if (m != M_STONE) continue;
                if (testb(anch, ni)) { anchored = true; break; }
                if (testb(vis, ni)) continue;
                setb(vis, ni);
                queue.emplace_back(nx, ny, nz);
            }
        }
        for (const IVec3& c : queue) clrb(vis, w.idx(c.x, c.y, c.z));

        if (anchored) {
            for (const IVec3& c : queue) {  // everything explored is connected to the anchor
                size_t i = w.idx(c.x, c.y, c.z);
                setb(anch, i);
                anchList.push_back(i);
            }
        } else {
            // A complete floating component: detach it into a rigid body.
            std::vector<IVec3> cells = queue;
            EditBox eb;
            for (const IVec3& c : cells) {
                w.setRaw(c.x, c.y, c.z, M_AIR);
                eb.add(c.x, c.y, c.z);
            }
            eb.flush(w);
            ph.createFromWorldCells(cells);
            created++;
            if (voxelsMoved) *voxelsMoved += (int)cells.size();
        }
    }
    for (size_t i : anchList) clrb(anch, i);
    return created;
}

// ------------------------------------------------------------------------------------------------
// Gun: carve
// ------------------------------------------------------------------------------------------------
CarveResult carveSphere(World& w, Physics& ph, const Vec3& c, float r, Rng& rng) {
    CarveResult res;
    std::vector<IVec3> removedStone;

    auto addDebris = [&](const Vec3& p, uint8_t m) {
        if (res.debris.size() < 48 && rng.below(5) == 0) {
            res.debris.push_back(p);
            res.debrisMat.push_back(m);
        }
    };

    EditBox carved;
    w.forEachInSphere(c, r, [&](int x, int y, int z) {
        uint8_t m = w.get(x, y, z);
        if (m == M_AIR || m == M_BEDROCK) return;  // Permanent Stone is immune
        if (m == M_STONE) removedStone.emplace_back(x, y, z);
        addDebris(Vec3(x + 0.5f, y + 0.5f, z + 0.5f), m);
        w.setRaw(x, y, z, M_AIR);
        carved.add(x, y, z);
        res.removedWorld++;
    });
    carved.flush(w);

    if (!removedStone.empty()) {
        static const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        std::vector<IVec3> seeds;
        for (const IVec3& rc : removedStone)
            for (const auto& d : nb) {
                int nx = rc.x + d[0], ny = rc.y + d[1], nz = rc.z + d[2];
                if (w.get(nx, ny, nz) == M_STONE) seeds.emplace_back(nx, ny, nz);
            }
        res.islandsCreated += detachIslands(w, ph, seeds, &res.islandVoxels);
    }

    // Rigid bodies: remove voxels inside the sphere, then split what is left into connected pieces.
    const size_t n = ph.bodies.size();
    for (size_t bi = 0; bi < n; bi++) {
        Body& b = *ph.bodies[bi];
        if (b.dead || length(b.pos - c) > b.radius + r + 1.0f) continue;
        const Vec3 cl = b.worldToLattice(c);
        const float r2 = r * r;
        std::vector<IVec3> remaining;
        remaining.reserve((size_t)b.count);
        int removed = 0;
        for (int k = 0; k < b.nz; k++)
            for (int j = 0; j < b.ny; j++)
                for (int i = 0; i < b.nx; i++) {
                    if (!b.occupiedAt(i, j, k)) continue;
                    const Vec3 cc(i + 0.5f, j + 0.5f, k + 0.5f);
                    if (lengthSq(cc - cl) <= r2) {
                        removed++;
                        addDebris(b.latticeToWorld(cc), M_STONE);
                    } else {
                        remaining.emplace_back(i, j, k);
                    }
                }
        if (removed) {
            res.removedBody += removed;
            const size_t before = ph.bodies.size();
            ph.setBodyCells(b, remaining);
            res.islandsCreated += (int)(ph.bodies.size() - before);
        }
    }

    ph.wakeNear(c, r + 24.0f);
    return res;
}

// ------------------------------------------------------------------------------------------------
// Creator: paint
// ------------------------------------------------------------------------------------------------
PaintResult paintSphere(World& w, Physics& ph, const Vec3& c, float r, uint8_t mat, const Vec3& pmin, const Vec3& pmax) {
    PaintResult res;
    std::vector<IVec3> cells;
    EditBox placed;

    w.forEachInSphere(c, r, [&](int x, int y, int z) {
        if (!w.inBounds(x, y, z)) return;
        const uint8_t cur = w.get(x, y, z);
        if (w.overlayAt(x, y, z)) return;  // occupied by a rigid body
        if (mat == M_SAND || mat == M_WATER) {
            if (cur != M_AIR) return;
        } else if (cur == M_STONE || cur == M_BEDROCK) {
            return;  // solids only replace empty space and loose material
        }
        if (mat != M_WATER && x + 1 > pmin.x && x < pmax.x && y + 1 > pmin.y && y < pmax.y && z + 1 > pmin.z && z < pmax.z)
            return;  // never entomb the player
        if (mat == M_STONE) {
            cells.emplace_back(x, y, z);
        } else {
            w.setRaw(x, y, z, mat);
            placed.add(x, y, z);
            res.placed++;
        }
    });
    placed.flush(w);
    if (mat != M_STONE) {
        if (mat == M_BEDROCK) ph.wakeNear(c, r + 24.0f);
        return res;
    }
    if (cells.empty()) return res;

    // Dense lookup for the new cells.
    IVec3 mn = cells[0], mx = cells[0];
    for (const IVec3& q : cells) {
        mn = IVec3(std::min(mn.x, q.x), std::min(mn.y, q.y), std::min(mn.z, q.z));
        mx = IVec3(std::max(mx.x, q.x), std::max(mx.y, q.y), std::max(mx.z, q.z));
    }
    const size_t dx = (size_t)(mx.x - mn.x + 1), dy = (size_t)(mx.y - mn.y + 1), dz = (size_t)(mx.z - mn.z + 1);
    std::vector<int> grid(dx * dy * dz, -1);
    auto gi = [&](const IVec3& q) { return (size_t)(q.x - mn.x) + dx * ((size_t)(q.y - mn.y) + dy * (size_t)(q.z - mn.z)); };
    auto inBox = [&](const IVec3& q) {
        return q.x >= mn.x && q.y >= mn.y && q.z >= mn.z && q.x <= mx.x && q.y <= mx.y && q.z <= mx.z;
    };
    for (size_t i = 0; i < cells.size(); i++) grid[gi(cells[i])] = (int)i;

    static const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<uint8_t> seen(cells.size(), 0);
    std::vector<int> queue;

    // 1) Cells connected to Permanent Stone / anchored Normal Stone (or the ground) join the static structure.
    for (size_t i = 0; i < cells.size(); i++) {
        const IVec3& q = cells[i];
        bool touch = q.y == 0;
        for (const auto& d : nb) {
            uint8_t m = w.get(q.x + d[0], q.y + d[1], q.z + d[2]);
            if (m == M_STONE || m == M_BEDROCK) touch = true;
        }
        if (touch && !seen[i]) {
            seen[i] = 1;
            queue.push_back((int)i);
        }
    }
    for (size_t h = 0; h < queue.size(); h++) {
        const IVec3 q = cells[queue[h]];
        for (const auto& d : nb) {
            IVec3 n(q.x + d[0], q.y + d[1], q.z + d[2]);
            if (!inBox(n)) continue;
            int j = grid[gi(n)];
            if (j >= 0 && !seen[j]) {
                seen[j] = 1;
                queue.push_back(j);
            }
        }
    }
    EditBox anchoredBox;
    for (int qi : queue) {
        const IVec3& q = cells[qi];
        w.setRaw(q.x, q.y, q.z, M_STONE);
        anchoredBox.add(q.x, q.y, q.z);
        res.staticStone++;
        res.placed++;
    }
    anchoredBox.flush(w);

    // 2) Remaining cells form free-floating pieces: merge into a touching body or spawn a new one.
    for (size_t s = 0; s < cells.size(); s++) {
        if (seen[s]) continue;
        std::vector<IVec3> comp;
        queue.clear();
        queue.push_back((int)s);
        seen[s] = 1;
        for (size_t h = 0; h < queue.size(); h++) {
            const IVec3 q = cells[queue[h]];
            comp.push_back(q);
            for (const auto& d : nb) {
                IVec3 n(q.x + d[0], q.y + d[1], q.z + d[2]);
                if (!inBox(n)) continue;
                int j = grid[gi(n)];
                if (j >= 0 && !seen[j]) {
                    seen[j] = 1;
                    queue.push_back(j);
                }
            }
        }
        res.placed += (int)comp.size();

        Body* target = nullptr;
        for (auto& up : ph.bodies) {
            Body& b = *up;
            if (b.dead || length(b.pos - c) > b.radius + r + 4.0f) continue;
            for (const IVec3& q : comp) {
                for (const auto& d : nb) {
                    Vec3 l = b.worldToLattice(Vec3(q.x + 0.5f + d[0], q.y + 0.5f + d[1], q.z + 0.5f + d[2]));
                    if (b.occupiedAt(floorToInt(l.x), floorToInt(l.y), floorToInt(l.z))) { target = &b; break; }
                }
                if (target) break;
            }
            if (target) break;
        }
        if (target) {
            std::vector<IVec3> all;
            target->collectCells(all);
            for (const IVec3& q : comp) {
                Vec3 l = target->worldToLattice(Vec3(q.x + 0.5f, q.y + 0.5f, q.z + 0.5f));
                all.emplace_back(floorToInt(l.x), floorToInt(l.y), floorToInt(l.z));
            }
            ph.setBodyCells(*target, all);
            res.mergedIntoBody += (int)comp.size();
        } else {
            ph.createFromWorldCells(comp);
            res.newBodies++;
        }
    }
    ph.wakeNear(c, r + 24.0f);
    return res;
}
