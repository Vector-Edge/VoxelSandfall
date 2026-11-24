// Fire, smoke and embers, game side. See Fire.h and FireWorld.cpp.
//
// Flames are particles. A burning coal / oil voxel (World::burning) keeps sending flames up from its exposed faces and puffs
// of smoke; a flame that touches coal lights it, one that touches oil lights all of it, one that touches water dies as steam.
// Free coal rocks (rigid bodies) burn too, voxel by voxel, with the same rules; when voxels of a rock burn away the rest is
// re-split into pieces. Smoke drifts up slowly, spreads out under ceilings and thins away.
#include <algorithm>
#include <cstdlib>
#include <unordered_map>

#include "Game.h"
#include "Profile.h"

namespace {
constexpr float kFlameBuoyancy = 120.0f;   // voxels / s^2 upwards
constexpr float kSmokeCeiling = 0.55f;     // how much of its rise a smoke particle keeps when it hits a ceiling (it slides sideways instead)
constexpr int kMaxFlamesPerFrame = 60;     // emission budget (the fire looks the same, the cost stays bounded)
constexpr int kMaxSmokePerFrame = 16;
constexpr size_t kSmokeCap = (size_t)fire::kMaxParticles - 3000;

inline float frand(Rng& r, float a, float b) { return r.range(a, b); }
inline bool insideWindowXZ(const Vec3& p) { return p.x >= 0.0f && p.x < (float)cfg::WORLD_X && p.z >= 0.0f && p.z < (float)cfg::WORLD_Z; }
}  // namespace

// ---- spawning -----------------------------------------------------------------------------------------------------
void Game::spawnFlame(const Vec3& p, float strength, float calm) {
    if (fx.size() >= (size_t)fire::kMaxParticles) return;
    FxParticle f;
    f.kind = FX_FLAME;
    f.pos = p;
    f.strength = strength;
    f.seed = rng.f();
    const float k = 0.8f + 0.5f * strength;
    f.vel = Vec3(frand(rng, -7, 7), frand(rng, 38, 78) * k * calm, frand(rng, -7, 7));
    f.life = frand(rng, 0.45f, 0.95f) * (0.85f + 0.4f * strength) / calm;
    f.size = frand(rng, 5.0f, 9.5f) * (0.8f + 0.55f * strength);
    fx.push_back(f);
    if (rng.f() < 0.07f) {  // a spark
        FxParticle e;
        e.kind = FX_EMBER;
        e.pos = p;
        e.strength = strength;
        e.seed = rng.f();
        e.vel = Vec3(frand(rng, -32, 32), frand(rng, 50, 125), frand(rng, -32, 32));
        e.life = frand(rng, 0.8f, 1.9f);
        e.size = frand(rng, 0.9f, 1.5f);
        if (fx.size() < (size_t)fire::kMaxParticles) fx.push_back(e);
    }
}

void Game::spawnSmoke(const Vec3& p, uint8_t kind, float scale) {
    if (fx.size() >= kSmokeCap) return;
    FxParticle s;
    s.kind = kind;
    s.pos = p;
    s.seed = rng.f();
    s.vel = Vec3(frand(rng, -3, 3), frand(rng, 10, 22), frand(rng, -3, 3));
    s.life = frand(rng, 4.2f, 8.0f);
    s.size = frand(rng, 4.0f, 7.0f) * scale;
    s.strength = scale;
    fx.push_back(s);
}

void Game::spawnFlames(const Vec3& c, float radius, int count, float strength) {
    for (int n = 0, tries = 0; n < count && tries < count * 8; tries++) {
        Vec3 d(frand(rng, -1, 1), frand(rng, -1, 1), frand(rng, -1, 1));
        if (lengthSq(d) > 1.0f) continue;
        const Vec3 p = c + d * radius;
        if (world.playerSolid(floorToInt(p.x), floorToInt(p.y), floorToInt(p.z))) continue;
        spawnFlame(p, strength, 0.5f);
        n++;
    }
}

void Game::spawnSmokeCloud(const Vec3& c, float radius, int count, uint8_t kind) {
    for (int n = 0, tries = 0; n < count && tries < count * 8; tries++) {
        Vec3 d(frand(rng, -1, 1), frand(rng, -1, 1), frand(rng, -1, 1));
        if (lengthSq(d) > 1.0f) continue;
        const Vec3 p = c + d * radius;
        if (world.playerSolid(floorToInt(p.x), floorToInt(p.y), floorToInt(p.z))) continue;
        spawnSmoke(p, kind, 1.0f);
        n++;
    }
}

// ---- igniting ------------------------------------------------------------------------------------------------------
bool Game::igniteBodyVoxel(Body& b, int i, int j, int k) {
    if (b.matAt(i, j, k) != M_COAL) return false;
    const Vec3 wp = b.latticeToWorld(Vec3((float)i + 0.5f, (float)j + 0.5f, (float)k + 0.5f));
    const int wx = floorToInt(wp.x), wy = floorToInt(wp.y), wz = floorToInt(wp.z);
    if (world.get(wx, wy, wz) == M_WATER || world.wet(wx, wy, wz)) return false;  // under water it cannot catch fire
    b.matRef(i, j, k) = M_BCOAL;
    anyBurningBodies_ = true;
    b.burn.push_back({i, j, k, rng.range(fire::kCoalBurnMin, fire::kCoalBurnMax)});
    b.burnDirty = true;
    b.wake();
    return true;
}

// A flame at p touches the coal rocks around it: each coal voxel next to it may catch (probability `prob`).
void Game::flameTouchesBodies(const Vec3& p, float prob) {
    for (auto& up : physics.bodies) {
        Body& b = *up;
        if (b.dead || b.count == 0) continue;
        const float rr = b.radius + 1.6f;
        if (lengthSq(b.pos - p) > rr * rr) continue;
        const Vec3 l = b.worldToLattice(p);
        const int ci = floorToInt(l.x), cj = floorToInt(l.y), ck = floorToInt(l.z);
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (b.matAt(ci + dx, cj + dy, ck + dz) == M_COAL && rng.f() < prob) igniteBodyVoxel(b, ci + dx, cj + dy, ck + dz);
    }
}

bool Game::igniteAt(int x, int y, int z) {
    if (world.ignite(x, y, z, &burnRep_)) return true;
    for (auto& up : physics.bodies) {  // a coal rock?
        Body& b = *up;
        const Vec3 l = b.worldToLattice(Vec3((float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f));
        if (igniteBodyVoxel(b, floorToInt(l.x), floorToInt(l.y), floorToInt(l.z))) return true;
    }
    return false;
}

// ---- burning rocks --------------------------------------------------------------------------------------------------
void Game::burnBodies(float sdt) {
    const float pSpread = std::min(1.0f, fire::kFlameLightsCoal * fire::kSpreadFactor * sdt);
    std::vector<Body*> cut;
    for (auto& up : physics.bodies) {
        Body& b = *up;
        if (b.dead || b.burn.empty()) continue;
        b.burnMeshT += sdt;
        b.burnCutT += sdt;
        bool anyBurnt = false;
        for (size_t k = 0; k < b.burn.size();) {
            BodyBurn bb = b.burn[k];
            const Vec3 wp = b.latticeToWorld(Vec3((float)bb.i + 0.5f, (float)bb.j + 0.5f, (float)bb.k + 0.5f));
            const int wx = floorToInt(wp.x), wy = floorToInt(wp.y), wz = floorToInt(wp.z);
            if (bb.left > 0 && (world.get(wx, wy, wz) == M_WATER || world.wet(wx, wy, wz))) {  // water put it out
                b.matRef(bb.i, bb.j, bb.k) = M_COAL;
                b.burn[k] = b.burn.back();
                b.burn.pop_back();
                b.burnDirty = true;
                spawnSmoke(wp, FX_STEAM, 1.0f);
                continue;
            }
            bb.left -= sdt;
            b.burn[k].left = bb.left;
            if (bb.left <= 0) {
                anyBurnt = true;
            } else {
                static const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (const auto& d : nb)
                    if (b.matAt(bb.i + d[0], bb.j + d[1], bb.k + d[2]) == M_COAL && rng.f() < pSpread) igniteBodyVoxel(b, bb.i + d[0], bb.j + d[1], bb.k + d[2]);
            }
            k++;
        }
        if (anyBurnt && b.burnCutT >= 0.25f) cut.push_back(&b);
        else if (b.burnDirty && b.burnMeshT >= 0.12f) {  // show the glow
            b.version++;
            b.burnDirty = false;
            b.burnMeshT = 0;
        }
    }
    for (Body* bp : cut) {  // cut out what has burnt away; what is left may fall into pieces
        Body& b = *bp;
        b.burnCutT = 0;
        b.burnMeshT = 0;
        std::vector<IVec3> remaining;
        remaining.reserve((size_t)b.count);
        std::vector<uint8_t> gone((size_t)b.nx * b.ny * b.nz, 0);
        for (const BodyBurn& bb : b.burn)
            if (bb.left <= 0) {
                gone[(size_t)bb.i + (size_t)b.nx * ((size_t)bb.j + (size_t)b.ny * (size_t)bb.k)] = 1;
                const Vec3 wp = b.latticeToWorld(Vec3((float)bb.i + 0.5f, (float)bb.j + 0.5f, (float)bb.k + 0.5f));
                if (rng.f() < 0.6f) spawnSmoke(wp, FX_SMOKE, 0.8f);
            }
        for (int k = 0; k < b.nz; k++)
            for (int j = 0; j < b.ny; j++)
                for (int i = 0; i < b.nx; i++)
                    if (b.occupiedAt(i, j, k) && !gone[(size_t)i + (size_t)b.nx * ((size_t)j + (size_t)b.ny * (size_t)k)]) remaining.emplace_back(i, j, k);
        b.burn.erase(std::remove_if(b.burn.begin(), b.burn.end(), [](const BodyBurn& x) { return x.left <= 0; }), b.burn.end());
        physics.setBodyCells(b, remaining);
    }
}

// ---- one fire step (30 Hz) ----------------------------------------------------------------------------------------------
void Game::fireStep(float sdt) {
    static std::vector<Vec3> steam;
    steam.clear();
    // 1. flames light what they touch, and die in water
    const bool anyFuel = world.counts[M_COAL] > 0 || world.counts[M_OIL] > 0;
    const bool anyWater = world.counts[M_WATER] > 0;
    for (FxParticle& f : fx) {
        if (f.kind != FX_FLAME || f.age >= f.life) continue;
        const int x = floorToInt(f.pos.x), y = floorToInt(f.pos.y), z = floorToInt(f.pos.z);
        if (anyWater && world.get(x, y, z) == M_WATER) {
            f.age = f.life;
            steam.push_back(f.pos);
            continue;
        }
        // a flame lights what is within a voxel of it; (the chance rate is for its whole reach: split over the 27 cells, with the
        // six faces next to its centre counting double)
        const float prob = fire::kFlameLightsCoal * f.strength * sdt;
        if (anyFuel)
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const uint8_t m = world.get(x + dx, y + dy, z + dz);
                        if (m == M_OIL) {
                            if (std::abs(dx) + std::abs(dy) + std::abs(dz) <= 1) world.ignite(x + dx, y + dy, z + dz, &burnRep_);
                        } else if (m == M_COAL) {
                            const int faces = std::abs(dx) + std::abs(dy) + std::abs(dz);
                            if (rng.f() < prob * (faces <= 1 ? 0.7f : 0.3f)) world.ignite(x + dx, y + dy, z + dz, &burnRep_);
                        }
                    }
        if (!physics.bodies.empty()) flameTouchesBodies(f.pos, prob * 0.4f);
    }
    for (const Vec3& p : steam) spawnSmoke(p, FX_STEAM, 0.9f);

    // 2. what is burning in the grid
    burnRep_.burntOut.clear();
    burnRep_.quenched.clear();
    world.burnStep(sdt, burnRep_);
    for (uint32_t c : burnRep_.quenched) {
        int x, y, z;
        unpackCell(c, x, y, z);
        spawnSmoke(Vec3(x + 0.5f, y + 1.0f, z + 0.5f), FX_STEAM, 1.1f);
        spawnSmoke(Vec3(x + 0.5f, y + 1.0f, z + 0.5f), FX_STEAM, 0.8f);
    }
    if (!burnRep_.burntOut.empty()) {
        static const int nb6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        std::vector<IVec3> seeds;
        for (uint32_t c : burnRep_.burntOut) {
            int x, y, z;
            unpackCell(c, x, y, z);
            if (rng.f() < 0.5f) spawnSmoke(Vec3(x + 0.5f, y + 0.8f, z + 0.5f), FX_SMOKE, 0.9f);
            for (const auto& d : nb6)
                if (isRigidMat(world.get(x + d[0], y + d[1], z + d[2]))) seeds.emplace_back(x + d[0], y + d[1], z + d[2]);
        }
        if (!seeds.empty()) detachIslands(world, physics, seeds);  // coal that was held up by what burnt away
    }
    burnRep_.caught.clear();

    // 3. burning rocks
    burnBodies(sdt);
}

// ---- flames and smoke from what is burning -----------------------------------------------------------------------------
void Game::emitFromFires(float dt) {
    int flames = kMaxFlamesPerFrame, smokes = kMaxSmokePerFrame;
    // grid voxels
    const size_t n = world.burning.size();
    if (n) {
        const float want = std::min((float)kMaxFlamesPerFrame, (float)n * 1.7f * dt);
        int cnt = (int)want + (rng.f() < want - std::floor(want) ? 1 : 0);
        for (int k = 0; k < cnt && flames > 0; k++) {
            const World::BurnCell& e = world.burning[(size_t)rng.below((int)n)];
            int x, y, z;
            unpackCell(e.cell, x, y, z);
            const uint8_t m = world.get(x, y, z);
            if (m != M_BCOAL && m != M_BOIL) continue;
            const bool oil = m == M_BOIL;
            Vec3 at;
            if (world.get(x, y + 1, z) == M_AIR && !world.overlayAt(x, y + 1, z)) at = Vec3(x + rng.f(), y + 1.0f, z + rng.f());
            else {
                static const int dd[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                const int* d = dd[rng.below(4)];
                if (world.get(x + d[0], y, z + d[1]) != M_AIR) continue;  // buried: nothing to see
                at = Vec3(x + 0.5f + d[0] * 0.8f, y + rng.f(), z + 0.5f + d[1] * 0.8f);
            }
            spawnFlame(at, oil ? 1.55f : 0.85f);
            flames--;
            if (smokes > 0 && rng.f() < (oil ? 0.34f : 0.2f)) {
                spawnSmoke(at + Vec3(0, 1.5f, 0), oil ? FX_SOOT : FX_SMOKE, oil ? 1.25f : 0.9f);
                smokes--;
            }
        }
    }
    // burning rocks
    for (auto& up : physics.bodies) {
        Body& b = *up;
        if (b.burn.empty() || b.dead) continue;
        const float want = std::min(30.0f, (float)b.burn.size() * 1.5f * dt);
        int cnt = (int)want + (rng.f() < want - std::floor(want) ? 1 : 0);
        for (int k = 0; k < cnt && flames > 0; k++) {
            const BodyBurn& bb = b.burn[(size_t)rng.below((int)b.burn.size())];
            if (bb.left <= 0) continue;
            // out of one of its faces that is not blocked (by the rock itself, another rock or the ground)
            static const int face[6][3] = {{0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}};
            const int start = rng.below(6);
            bool done = false;
            for (int q = 0; q < 6 && !done; q++) {
                const int* d = face[(start + q) % 6];
                if (b.matAt(bb.i + d[0], bb.j + d[1], bb.k + d[2]) != 0) continue;
                const Vec3 wp = b.latticeToWorld(Vec3((float)bb.i + 0.5f + 0.9f * d[0], (float)bb.j + 0.5f + 0.9f * d[1], (float)bb.k + 0.5f + 0.9f * d[2]));
                if (world.playerSolid(floorToInt(wp.x), floorToInt(wp.y), floorToInt(wp.z))) continue;
                spawnFlame(wp, 0.8f);
                flames--;
                if (smokes > 0 && rng.f() < 0.2f) {
                    spawnSmoke(wp + Vec3(0, 1.5f, 0), FX_SMOKE, 0.85f);
                    smokes--;
                }
                done = true;
            }
        }
    }
}

// ---- particle motion (every frame) --------------------------------------------------------------------------------------
void Game::updateFx(float dt) {
    const float t = time;
    for (size_t i = 0; i < fx.size();) {
        FxParticle& f = fx[i];
        f.age += dt;
        bool dead = f.age >= f.life;
        if (!dead) {
            const float life01 = f.age / f.life;
            switch (f.kind) {
                case FX_FLAME: {
                    f.vel.y += kFlameBuoyancy * dt;
                    f.vel.x += std::sin(f.pos.y * 0.21f + f.seed * 40.0f + t * 9.0f) * 55.0f * dt;
                    f.vel.z += std::cos(f.pos.y * 0.19f + f.seed * 31.0f + t * 8.0f) * 55.0f * dt;
                    const float drag = 1.0f / (1.0f + 1.7f * dt);
                    f.vel *= drag;
                    break;
                }
                case FX_EMBER:
                    f.vel.y -= cfg::GRAVITY * 0.28f * dt;
                    f.vel.x += std::sin(t * 7.0f + f.seed * 50.0f) * 25.0f * dt;
                    f.vel.z += std::cos(t * 6.0f + f.seed * 37.0f) * 25.0f * dt;
                    f.vel *= 1.0f / (1.0f + 0.7f * dt);
                    break;
                default: {  // smoke: rises slowly, loses its lift, wanders on slow currents
                    const float rise = (f.kind == FX_STEAM ? 17.0f : 13.0f) * (1.0f - 0.75f * life01);
                    f.vel.y += (rise - f.vel.y) * std::min(1.0f, dt * 0.8f);
                    f.vel.x += std::sin(f.pos.y * 0.043f + f.seed * 17.0f + t * 0.8f) * 7.0f * dt;
                    f.vel.z += std::cos(f.pos.y * 0.051f + f.seed * 11.0f + t * 0.7f) * 7.0f * dt;
                    f.vel.x *= 1.0f - 0.35f * dt;
                    f.vel.z *= 1.0f - 0.35f * dt;
                    break;
                }
            }
            const Vec3 np = f.pos + f.vel * dt;
            const int nx = floorToInt(np.x), ny = floorToInt(np.y), nz = floorToInt(np.z);
            if (world.playerSolid(nx, ny, nz)) {
                if (f.kind == FX_FLAME) {  // a flame licks along a wall or a ceiling instead of stopping dead
                    Vec3 p = f.pos;
                    auto blocked = [&](float x, float y, float z) { return world.playerSolid(floorToInt(x), floorToInt(y), floorToInt(z)); };
                    if (!blocked(np.x, p.y, p.z)) p.x = np.x; else f.vel.x *= -0.2f;
                    if (!blocked(p.x, np.y, p.z)) p.y = np.y; else f.vel.y *= 0.3f;
                    if (!blocked(p.x, p.y, np.z)) p.z = np.z; else f.vel.z *= -0.2f;
                    f.pos = p;
                    if (blocked(p.x, p.y, p.z)) dead = true;
                } else if (f.kind == FX_EMBER) {
                    dead = true;
                } else {  // smoke slides along it: along a ceiling it spreads sideways (slowly)
                    if (f.vel.y > 0) {
                        f.vel.y = 0;
                        f.vel.x += std::cos(f.seed * 97.0f + t * 0.3f) * 16.0f * dt;
                        f.vel.z += std::sin(f.seed * 83.0f + t * 0.3f) * 16.0f * dt;
                    } else {
                        f.vel.y *= 0.5f;
                    }
                    f.vel.x *= 1.0f / (1.0f + 1.6f * dt);
                    f.vel.z *= 1.0f / (1.0f + 1.6f * dt);
                    if (!world.playerSolid(nx, floorToInt(f.pos.y), floorToInt(f.pos.z))) f.pos.x = np.x;
                    else f.vel.x *= -0.3f;
                    if (!world.playerSolid(floorToInt(f.pos.x), floorToInt(f.pos.y), nz)) f.pos.z = np.z;
                    else f.vel.z *= -0.3f;
                    if (!world.playerSolid(floorToInt(f.pos.x), ny, floorToInt(f.pos.z))) f.pos.y = np.y;
                }
            } else {
                f.pos = np;
            }
            if (f.pos.y < -20.0f || f.pos.y > cfg::WORLD_Y + 200.0f) dead = true;
            if (!insideWindowXZ(f.pos)) dead = true;
        }
        if (dead) {
            fx[i] = fx.back();
            fx.pop_back();
        } else {
            i++;
        }
    }
}

// ---- glows: the flames merged into at most 16 lights ---------------------------------------------------------------------------
void Game::updateFireLights() {
    fireLights.clear();
    if (fx.empty()) return;
    struct Acc {
        Vec3 sum;
        float w = 0;
    };
    static std::unordered_map<uint64_t, Acc> grid;
    grid.clear();
    constexpr float cell = 26.0f;
    const size_t stride = fx.size() > 6000 ? 3 : 1;
    for (size_t i = 0; i < fx.size(); i += stride) {
        const FxParticle& f = fx[i];
        if (f.kind != FX_FLAME) continue;
        const float k = 1.0f - f.age / f.life;
        const float w = f.strength * k * (f.size * 0.12f) * (float)stride;
        const int cx = floorToInt(f.pos.x / cell), cy = floorToInt(f.pos.y / cell), cz = floorToInt(f.pos.z / cell);
        const uint64_t key = ((uint64_t)(uint32_t)(cx + 4096) << 40) | ((uint64_t)(uint32_t)(cy + 4096) << 20) | (uint64_t)(uint32_t)(cz + 4096);
        Acc& a = grid[key];
        a.sum += f.pos * w;
        a.w += w;
    }
    std::vector<std::pair<float, Vec3>> top;
    top.reserve(grid.size());
    for (auto& kv : grid)
        if (kv.second.w > 0.05f) top.emplace_back(kv.second.w, kv.second.sum * (1.0f / kv.second.w));
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < top.size() && (int)i < fire::kMaxLights; i++) {
        FireLight l;
        l.pos = top[i].second + Vec3(0, 3.0f, 0);
        l.power = std::min(top[i].first, 14.0f);
        l.radius = 34.0f + 20.0f * std::sqrt(l.power);
        fireLights.push_back(l);
    }
}

// ---- the whole thing -------------------------------------------------------------------------------------------------------
void Game::updateFire(float dt) {
    if (fx.empty() && world.burning.empty() && !anyBurningBodies_) {
        fireAcc_ = 0;
        fireLights.clear();
        return;
    }
    fireAcc_ += dt;
    const float sdt = 1.0f / fire::kStepHz;
    int steps = 0;
    while (fireAcc_ >= sdt && steps < 3) {
        fireStep(sdt);
        fireAcc_ -= sdt;
        steps++;
    }
    if (fireAcc_ > 3.0f * sdt) fireAcc_ = 0;
    anyBurningBodies_ = false;
    for (auto& up : physics.bodies)
        if (!up->burn.empty()) {
            anyBurningBodies_ = true;
            break;
        }
    emitFromFires(dt);
    updateFx(dt);
    updateFireLights();
}

size_t Game::flameCount() const {
    size_t n = 0;
    for (const FxParticle& f : fx) n += f.kind == FX_FLAME;
    return n;
}

size_t Game::smokeCount() const {
    size_t n = 0;
    for (const FxParticle& f : fx) n += f.kind == FX_SMOKE || f.kind == FX_STEAM || f.kind == FX_SOOT;
    return n;
}
