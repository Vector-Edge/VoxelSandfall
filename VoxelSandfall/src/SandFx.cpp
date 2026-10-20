// Player <-> sand / water interaction. Sand and water react to the player; stone does not (it is rigid).
// Everything is mass conserving where it can be: displaced material is re-deposited as real voxels (instantly,
// or as flying grains / droplets that settle where they land), except for a small "compaction" share of
// footprints and craters which simply vanishes. The harder or faster the player hits the surface, the bigger the
// splash.
//
//   sand   - footsteps dig dents and kick grains; hard landings make craters; a steady dust spray while running;
//            walking into a pile too steep to climb shoves it aside (wading)
//   water  - the player's body displaces water (it is solid to water), so you wade, swim and splash instead of
//            walking "through" it; displaced water is thrown out as droplets / a bow wave and the pool then
//            levels itself back in behind you
//
// Re-deposited material goes back into the cellular automaton, so ridges slump, dents fill, wakes close and
// splashes run back downhill according to the normal sand / water rules.
#include <algorithm>

#include "Game.h"

namespace {
constexpr size_t kMaxParticles = (size_t)cfg::MAX_PARTICLES;
}


int Game::grainsInFlight() const {
    int n = 0;
    for (const Particle& p : particles) n += p.grain ? 1 : 0;
    return n;
}

// First free cell above the topmost solid cell of the column, scanning down from yFrom.
int Game::columnTop(int x, int z, int yFrom) const {
    for (int y = std::min(yFrom, world.SY - 1); y >= 0; --y)
        if (world.playerSolid(x, y, z)) return y + 1;
    return 0;
}

// Like columnTop, but water also counts as a surface (grains and droplets land on top of it).
int Game::columnSurface(int x, int z, int yFrom) const {
    for (int y = std::min(yFrom, world.SY - 1); y >= 0; --y)
        if (!world.cellFree(x, y, z)) return y + 1;
    return 0;
}

// Put one voxel of `mat` at (x,y,z) if that cell is free and not inside the player; grains that would land inside
// the player's box are moved out sideways first. Returns false if nowhere sensible was found (the grain is lost).
bool Game::placeGrain(int x, int y, int z, uint8_t mat) {
    Vec3 mn, mx;
    player.aabb(mn, mx);
    if (mat == M_WATER) {  // water is only kept out of the torso, sand out of the whole body box
        mn.x = player.pos.x - cfg::PLAYER_CORE * 0.5f;
        mx.x = player.pos.x + cfg::PLAYER_CORE * 0.5f;
        mn.z = player.pos.z - cfg::PLAYER_CORE * 0.5f;
        mx.z = player.pos.z + cfg::PLAYER_CORE * 0.5f;
    }
    mn -= Vec3(0.6f, 0.0f, 0.6f);
    mx += Vec3(0.6f, 0.0f, 0.6f);
    auto inBox = [&](int cx, int cy, int cz) {
        return cx + 1 > mn.x && cx < mx.x && cy + 1 > mn.y && cy < mx.y && cz + 1 > mn.z && cz < mx.z;
    };
    if (inBox(x, y, z)) {
        // Slide outwards (away from the player's centre) until clear of the box, then drop onto the surface.
        float dx = (x + 0.5f) - player.pos.x, dz = (z + 0.5f) - player.pos.z;
        float l = std::sqrt(dx * dx + dz * dz);
        if (l < 0.01f) { dx = 1; dz = 0; l = 1; }
        dx /= l;
        dz /= l;
        float px = (float)x + 0.5f, pz = (float)z + 0.5f;
        for (int i = 0; i < 80 && inBox(floorToInt(px), y, floorToInt(pz)); i++) {
            px += dx;
            pz += dz;
        }
        x = floorToInt(px);
        z = floorToInt(pz);
        y = columnSurface(x, z, y + 8);
    }
    // Water may have to climb a tall stack of other falling water (its own cell can be refilled while it flies).
    const int maxTries = mat == M_WATER ? 48 : 6;
    for (int tries = 0; tries < maxTries; tries++, y++) {
        if (!world.inBounds(x, y, z)) break;
        if (world.get(x, y, z) == M_AIR && !world.overlayAt(x, y, z) && !inBox(x, y, z)) {
            if (mat == M_WATER) world.setWaterQuiet(x, y, z);
            else world.set(x, y, z, mat);
            return true;
        }
    }
    if (mat == M_WATER) {
        // Still nothing: the nearest column around it with room (water must not be lost).
        const int y0 = std::max(0, y - maxTries);
        for (int r = 1; r <= 8; r++)
            for (int k = 0; k < 8; k++) {
                static const int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                const int nx = x + kDx[k] * r, nz = z + kDz[k] * r;
                const int ny = columnSurface(nx, nz, y0 + 12);
                if (world.inBounds(nx, ny, nz) && world.get(nx, ny, nz) == M_AIR && !world.overlayAt(nx, ny, nz) && !inBox(nx, ny, nz)) {
                    world.setWaterQuiet(nx, ny, nz);
                    return true;
                }
            }
    }
    if (mat == M_SAND) {
        // A grain that sank into water: it takes the place of one water voxel, which pops up to the surface.
        const int y0 = std::max(0, y - 6);
        if (world.get(x, y0, z) == M_WATER) {
            int ya = y0;
            while (world.inBounds(x, ya, z) && world.get(x, ya, z) == M_WATER) ya++;
            if (world.inBounds(x, ya, z) && world.cellFree(x, ya, z)) {
                world.set(x, ya, z, M_WATER);
                world.set(x, y0, z, M_SAND);
                return true;
            }
        }
    }
    lostGrains++;
    snprintf(lostInfo, sizeof(lostInfo), "mat %d at %d,%d,%d (start y %d): cell=%d overlay=%d inBounds=%d", (int)mat, x, y, z, y - maxTries, (int)world.get(x, y - 1, z), (int)world.overlayAt(x, y - 1, z), (int)world.inBounds(x, y - 1, z));
    return false;
}

void Game::settleGrain(const Vec3& pos, uint8_t mat) { placeGrain(floorToInt(pos.x), floorToInt(pos.y), floorToInt(pos.z), mat); }

// Throws a displaced voxel out horizontally by roughly `range` voxels (plus `lift` extra upward speed). Visible
// ones fly as particles; the rest are placed straight away (so big events cannot flood the particle list).
void Game::launchGrain(const Vec3& from, const Vec3& dirOut, float range, bool visible, uint8_t mat, float lift) {
    if (visible && particles.size() < kMaxParticles) {
        Particle p;
        p.grain = true;
        p.mat = mat;
        p.pos = from;
        p.size = 1.0f;
        p.life = 5.0f;
        // 45 degree launch: range = v^2 / g.
        const float v = std::sqrt(std::max(range, 1.0f) * cfg::GRAVITY) * rng.range(0.85f, 1.15f) * splashScale_;
        const float vh = v * 0.70f, vy = v * 0.74f + lift;
        p.vel = Vec3(dirOut.x * vh + rng.range(-12.0f, 12.0f), vy, dirOut.z * vh + rng.range(-12.0f, 12.0f));
        particles.push_back(p);
        return;
    }
    // Direct placement. A target inside a wall, or above/inside the player, is rejected and another spot is tried
    // (later attempts spread out from the player's box instead of from the voxel's own position).
    const float reach = (mat == M_WATER ? cfg::PLAYER_CORE : cfg::PLAYER_W) * 0.71f + 2.0f;  // just beyond the corners of the player's box
    for (int attempt = 0; attempt < 14; attempt++) {
        float tx_f, tz_f;
        if (attempt == 0) {
            const float r = range * rng.range(0.7f, 1.3f);
            tx_f = from.x + dirOut.x * r;
            tz_f = from.z + dirOut.z * r;
        } else {
            const float a = rng.range(0.0f, 2.0f * kPi), dd = reach + rng.range(0.0f, 10.0f + 6.0f * (float)attempt);
            tx_f = player.pos.x + std::cos(a) * dd;
            tz_f = player.pos.z + std::sin(a) * dd;
        }
        const int tx = floorToInt(tx_f), tz = floorToInt(tz_f);
        if (world.boxOn && tx >= world.boxMin[0] - 1 && tx <= world.boxMax[0] + 1 && tz >= world.boxMin[2] - 1 && tz <= world.boxMax[2] + 1) continue;
        // Only columns whose surface is at or below the ceiling are acceptable (this keeps displaced water off
        // the tops of walls, and sand off tall obstacles).
        const int yTop = std::min(world.SY - 1, placeCeiling_ >= 0 ? placeCeiling_ : floorToInt(from.y) + 40);
        if (!world.inBounds(tx, yTop, tz) || !world.cellFree(tx, yTop, tz)) continue;
        if (placeGrain(tx, columnSurface(tx, tz, yTop), tz, mat)) return;
    }
    // Every target was refused (walls, bodies, the player): water must not vanish, so it goes back where it came from.
    if (mat == M_WATER) placeGrain(floorToInt(from.x), floorToInt(from.y), floorToInt(from.z), mat);
}

// Removes sand around (cx,cz) from the surface downwards: `depth` layers in the middle, tapering to 0 at the edge.
// Calls onRemoved(cellCentre) for every voxel taken. Returns the number removed.
template <class F>
int Game::digSand(float cx, float cz, float radius, float depth, int hintY, F&& onRemoved) {
    struct Cell { int x, y, z; };
    static std::vector<Cell> removed;
    removed.clear();
    const int x0 = floorToInt(cx - radius), x1 = floorToInt(cx + radius);
    const int z0 = floorToInt(cz - radius), z1 = floorToInt(cz + radius);
    int bx0 = 1 << 30, by0 = 1 << 30, bz0 = 1 << 30, bx1 = -1, by1 = -1, bz1 = -1;
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++) {
            const float dx = x + 0.5f - cx, dz = z + 0.5f - cz;
            const float r2 = (dx * dx + dz * dz) / (radius * radius);
            if (r2 >= 1.0f) continue;
            // topmost sand voxel near the foot
            int top = -1;
            for (int y = hintY + 2; y >= std::max(0, hintY - 10); --y) {
                const uint8_t m = world.get(x, y, z);
                if (m == M_SAND) { top = y; break; }
                if (m >= M_STONE) break;
            }
            if (top < 0) continue;
            const float d = depth * (1.0f - r2);
            int layers = (int)d + (rng.f() < d - std::floor(d) ? 1 : 0);
            for (int l = 0; l < layers; l++) {
                const int y = top - l;
                if (y < 0 || world.get(x, y, z) != M_SAND) break;
                world.setRaw(x, y, z, M_AIR);
                removed.push_back({x, y, z});
                bx0 = std::min(bx0, x); by0 = std::min(by0, y); bz0 = std::min(bz0, z);
                bx1 = std::max(bx1, x); by1 = std::max(by1, y); bz1 = std::max(bz1, z);
            }
        }
    if (removed.empty()) return 0;
    world.notifyBox(bx0, by0, bz0, bx1, by1, bz1);
    for (const Cell& c : removed) onRemoved(Vec3(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f));
    return (int)removed.size();
}

void Game::sandFootstep(int side, bool sprint) {
    const Vec3 right = player.rightDir();
    Vec3 move(player.vel.x, 0, player.vel.z);
    const float speed = length(move);
    move = speed > 1e-2f ? move / speed : Vec3(-std::sin(player.yaw), 0, -std::cos(player.yaw));
    const float fx = player.pos.x + right.x * (float)side * 10.0f, fz = player.pos.z + right.z * (float)side * 10.0f;
    // Bigger and deeper the faster the player is going.
    const float k = clampf(speed / cfg::SPRINT_SPEED, 0.35f, 1.3f);
    const float radius = 7.0f + 4.5f * k, depth = 2.4f + 3.0f * k;
    const float savedScale = splashScale_;
    splashScale_ = 0.8f + 0.45f * k;
    digSand(fx, fz, radius, depth, floorToInt(player.pos.y) - 1, [&](const Vec3& from) {
        if (rng.f() < 0.15f) return;  // compaction: the foot packs a little sand down
        Vec3 out(from.x - fx, 0, from.z - fz);
        out = lengthSq(out) > 1e-4f ? normalize(out) : Vec3(rng.range(-1, 1), 0, rng.range(-1, 1));
        // Kicked mostly backwards and to the sides of the path, so it builds a berm along the footprints.
        Vec3 sideOut = right * (float)side;
        Vec3 dir = normalize(-move * 0.9f + sideOut * 0.8f + out * 0.35f + Vec3(rng.range(-0.3f, 0.3f), 0, rng.range(-0.3f, 0.3f)));
        launchGrain(from, dir, rng.range(7.0f, 20.0f) * (0.7f + 0.7f * k), rng.f() < 0.7f, M_SAND, 28.0f * k);
    });
    splashScale_ = savedScale;
}

void Game::sandCrater(float impact) {
    const float radius = std::clamp(5.0f + impact * 0.03f, 7.0f, 20.0f);
    const float depth = std::clamp(1.8f + impact / 110.0f, 2.5f, 7.0f);
    const float cx = player.pos.x, cz = player.pos.z;
    const float savedScale = splashScale_;
    splashScale_ = clampf(impact / 320.0f, 0.8f, 1.35f);
    digSand(cx, cz, radius, depth, floorToInt(player.pos.y) - 1, [&](const Vec3& from) {
        if (rng.f() < 0.15f) return;
        Vec3 out(from.x - cx, 0, from.z - cz);
        out = lengthSq(out) > 1e-4f ? normalize(out) : Vec3(rng.range(-1, 1), 0, rng.range(-1, 1));
        launchGrain(from, normalize(out + Vec3(rng.range(-0.25f, 0.25f), 0, rng.range(-0.25f, 0.25f))), radius * 0.5f + rng.range(6.0f, 20.0f),
                    rng.f() < 0.7f, M_SAND, impact * 0.10f);
    });
    splashScale_ = savedScale;
}

bool Game::sandPlow(const IVec3& mn, const IVec3& mx, int axis, int sign) {
    const int feetCell = floorToInt(player.pos.y);
    const int maxY = feetCell + (int)cfg::PLOW_HEIGHT;
    // Everything in the way must be loose sand low enough to push; anything else is a wall.
    for (int z = mn.z; z <= mx.z; z++)
        for (int y = mn.y; y <= mx.y; y++)
            for (int x = mn.x; x <= mx.x; x++) {
                if (!world.playerSolid(x, y, z)) continue;
                if (world.overlayAt(x, y, z) || world.get(x, y, z) != M_SAND || y > maxY) return false;
            }
    struct Cell { int x, y, z; };
    static std::vector<Cell> cells;
    cells.clear();
    int bx0 = 1 << 30, by0 = 1 << 30, bz0 = 1 << 30, bx1 = -1, by1 = -1, bz1 = -1;
    for (int z = mn.z; z <= mx.z; z++)
        for (int y = mn.y; y <= mx.y; y++)
            for (int x = mn.x; x <= mx.x; x++) {
                if (world.get(x, y, z) != M_SAND) continue;
                world.setRaw(x, y, z, M_AIR);
                cells.push_back({x, y, z});
                bx0 = std::min(bx0, x); by0 = std::min(by0, y); bz0 = std::min(bz0, z);
                bx1 = std::max(bx1, x); by1 = std::max(by1, y); bz1 = std::max(bz1, z);
            }
    if (cells.empty()) return true;
    world.notifyBox(bx0, by0, bz0, bx1, by1, bz1);

    const float speed = std::sqrt(player.vel.x * player.vel.x + player.vel.z * player.vel.z);
    const float savedScale = splashScale_;
    splashScale_ = clampf(speed / cfg::WALK_SPEED, 0.8f, 1.4f);
    const int lat = axis == 0 ? 2 : 0;  // the horizontal axis perpendicular to the direction of travel
    const float centre = lat == 0 ? player.pos.x : player.pos.z;
    const float halfW = cfg::PLAYER_W * 0.5f;
    for (const Cell& c : cells) {
        const float l = (lat == 0 ? c.x : c.z) + 0.5f - centre;
        const float side = std::fabs(l) < 1.0f ? (rng.f() < 0.5f ? 1.0f : -1.0f) : (l > 0 ? 1.0f : -1.0f);
        Vec3 dir(0, 0, 0);
        dir[lat] = side * 0.9f;
        dir[axis] = (float)sign * 0.45f;
        dir = normalize(dir);
        const float range = std::max(0.0f, halfW + 3.0f - std::fabs(l)) + rng.range(3.0f, 14.0f);
        const bool visible = plowParticleBudget_ > 0;
        if (visible) plowParticleBudget_--;
        launchGrain(Vec3(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f), dir, range, visible, M_SAND, speed * 0.1f);
    }
    splashScale_ = savedScale;
    return true;
}

// Per-frame hook: landing craters, footsteps and a steady dust spray while moving over sand.
void Game::updateSandInteraction(float dt, const GameInput& in, float velYBefore, bool wasOnGround) {
    (void)in;
    if (!player.onGround) {
        stepAccum_ = 0;
        return;
    }
    const bool onSand = world.get(floorToInt(player.pos.x), floorToInt(player.pos.y - 0.05f), floorToInt(player.pos.z)) == M_SAND;
    if (!onSand) return;
    if (!wasOnGround && velYBefore < -90.0f) {
        sandCrater(-velYBefore);
        stepAccum_ = 0;
        return;
    }
    const float speed = std::sqrt(player.vel.x * player.vel.x + player.vel.z * player.vel.z);
    if (speed < 25.0f) return;

    // Dust: a few grains flicked off the surface around the feet every frame, more the faster you go.
    {
        const Vec3 move = Vec3(player.vel.x, 0, player.vel.z) / speed;
        const float expect = speed / 70.0f;
        int n = (int)expect + (rng.f() < expect - std::floor(expect) ? 1 : 0);
        for (int i = 0; i < n; i++) {
            const float a = rng.range(0.0f, 2.0f * kPi), r = rng.range(2.0f, 15.0f);
            const int x = floorToInt(player.pos.x + std::cos(a) * r), z = floorToInt(player.pos.z + std::sin(a) * r);
            for (int y = floorToInt(player.pos.y) + 1; y >= std::max(0, floorToInt(player.pos.y) - 6); --y) {
                if (world.get(x, y, z) != M_SAND) continue;
                world.set(x, y, z, M_AIR);
                const Vec3 from(x + 0.5f, y + 0.5f, z + 0.5f);
                const Vec3 dir = normalize(-move * 0.8f + Vec3(rng.range(-0.6f, 0.6f), 0, rng.range(-0.6f, 0.6f)));
                const float saved = splashScale_;
                splashScale_ = clampf(speed / 220.0f, 0.7f, 1.3f);
                launchGrain(from, dir, rng.range(8.0f, 20.0f), true, M_SAND, 14.0f);
                splashScale_ = saved;
                break;
            }
        }
    }

    const bool sprint = speed > 230.0f;
    stepAccum_ += speed * dt;
    if (stepAccum_ >= (sprint ? 54.0f : 40.0f)) {
        stepAccum_ = 0;
        stepSide_ = -stepSide_;
        sandFootstep(stepSide_, sprint);
    }
}

// ----------------------------------------------------------------------------------------------------------
// Water
// ----------------------------------------------------------------------------------------------------------

// The player is solid to water: whatever water is inside the player's box gets pushed out. How hard it is thrown
// depends on how fast the player is moving (`velBefore` is the velocity at the start of the frame).
void Game::processSplashes() {
    std::vector<World::SplashEvent>& ev = world.splashes;
    if (ev.empty()) return;
    const size_t n = ev.size();
    // Ripples: a bounded number of impulses, each one standing in for several events (a big pour reports thousands).
    const size_t stride = std::max<size_t>(1, n / 250);
    const float amp = std::min(std::sqrt((float)stride), 3.0f);
    for (size_t i = 0; i < n; i++) {
        const World::SplashEvent& e = ev[i];
        const float power = (float)e.power;
        if (e.droplet) {
            // This voxel was taken out of the grid: it flies up and outwards and lands as a voxel again.
            if (particles.size() < kMaxParticles) {
                Particle p;
                p.grain = true;
                p.mat = M_WATER;
                p.pos = Vec3(e.x, e.y + 0.5f, e.z);
                p.size = 1.0f;
                p.life = 5.0f;
                const float a = rng.range(0.0f, 2.0f * kPi), r = rng.range(8.0f, 48.0f);
                p.vel = Vec3(std::cos(a) * r, rng.range(70.0f, 170.0f) * (0.75f + 0.12f * power), std::sin(a) * r);
                particles.push_back(p);
            } else {
                settleGrain(Vec3(e.x, e.y + 0.5f, e.z), M_WATER);
            }
        } else if (power >= 2 && particles.size() < 1800 && rng.below(100) < 5) {
            // A little extra spray that is only for show (it vanishes when it falls back into the water).
            Particle p;
            p.grain = false;
            p.mat = M_WATER;
            p.pos = Vec3(e.x, e.y + 1.0f, e.z);
            p.size = rng.range(0.5f, 0.9f);
            p.life = rng.range(0.5f, 0.9f);
            const float a = rng.range(0.0f, 2.0f * kPi), r = rng.range(5.0f, 30.0f);
            p.vel = Vec3(std::cos(a) * r, rng.range(60.0f, 150.0f), std::sin(a) * r);
            particles.push_back(p);
        }
        if (i % stride == 0) waterField.impulse(e.x, e.z, 0.07f * power * amp, 1.4f + 0.3f * power, 0.07f * power);
    }
    ev.clear();
}

void Game::updateWaterInteraction(float dt, const Vec3& velBefore) {
    (void)dt;
    // The torso (a core inside the 50-wide collision box) is what displaces water and keeps sand out.
    const float hwc = cfg::PLAYER_CORE * 0.5f;
    const int x0 = floorToInt(player.pos.x - hwc + 0.005f), x1 = (int)std::ceil(player.pos.x + hwc - 0.005f) - 1;
    const int y0 = floorToInt(player.pos.y + 0.005f), y1 = (int)std::ceil(player.pos.y + cfg::PLAYER_H - 0.005f) - 1;
    const int z0 = floorToInt(player.pos.z - hwc + 0.005f), z1 = (int)std::ceil(player.pos.z + hwc - 0.005f) - 1;
    world.setPlayerBox(x0, y0, z0, x1, y1, z1);
    if (world.counts[M_WATER] == 0) return;

    static std::vector<WaterCell> evicted;
    evicted.clear();
    int bx0 = 1 << 30, by0 = 1 << 30, bz0 = 1 << 30, bx1 = -1, by1 = -1, bz1 = -1;
    const size_t kMaxPerFrame = 60000;
    for (int z = z0; z <= z1 && evicted.size() < kMaxPerFrame; z++)
        for (int x = x0; x <= x1; x++) {
            size_t first = evicted.size();
            // The whole column is scanned: displaced water heaped up beside the player can hang inside the box
            // above cells that were cleared earlier (the box blocks it from falling).
            for (int y = y0; y <= std::min(y1, y0 + 90); y++) {
                if (world.get(x, y, z) != M_WATER) continue;
                world.setRaw(x, y, z, M_AIR);
                evicted.push_back({x, y, z, false});
                bx0 = std::min(bx0, x); by0 = std::min(by0, y); bz0 = std::min(bz0, z);
                bx1 = std::max(bx1, x); by1 = std::max(by1, y); bz1 = std::max(bz1, z);
            }
            // The top few voxels of each column are the ones that splash.
            for (size_t k = evicted.size(); k > first && k + 3 > evicted.size(); k--) evicted[k - 1].surface = true;
        }
    // The cells the body left since last frame (they are outside the new box): they can be filled again at once.
    static std::vector<uint32_t> vacated, hollowNow;
    vacated.clear();
    hollowNow.clear();
    for (uint32_t i : hollow_) {
        int hx, hy, hz;
        world.cellOfIndex(i, hx, hy, hz);
        if (world.inPlayerBox(hx, hy, hz)) hollowNow.push_back(i);
        else if ((world.data[i] & VOX_MASK) == M_AIR && !world.overlayAt(hx, hy, hz)) vacated.push_back(i);
    }
    for (const WaterCell& c : evicted) hollowNow.push_back((uint32_t)world.idx(c.x, c.y, c.z));
    hollow_.swap(hollowNow);
    if (evicted.empty()) return;
    world.notifyBoxWater(bx0, by0, bz0, bx1, by1, bz1);

    // Walking through water: the deep water the front of the body pushes aside simply ends up where the back of the body
    // was - no pit left behind to cascade shut, no bow wave under the surface. Only the voxels at the surface are thrown
    // out as splash (and everything, when there is no empty trail yet: the plunge, the first step in).
    size_t carried = 0;
    for (WaterCell& c : evicted) {
        if (c.surface || carried >= vacated.size()) continue;
        int vx, vy, vz;
        world.cellOfIndex(vacated[carried++], vx, vy, vz);
        world.setWaterQuiet(vx, vy, vz);
        c.x = -1;  // consumed
    }
    if (carried) evicted.erase(std::remove_if(evicted.begin(), evicted.end(), [](const WaterCell& c) { return c.x < 0; }), evicted.end());
    if (evicted.empty()) return;

    throwWater(evicted, player.pos, velBefore, cfg::PLAYER_CORE * 0.5f, by1, 1.0f);
}

void Game::throwWater(const std::vector<WaterCell>& cells, const Vec3& centre, const Vec3& vel, float halfWidth, int topY, float rippleScale) {
    const float hspeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
    const float speed = std::max(hspeed, std::fabs(vel.y));  // a plunge counts as much as a sprint
    lastSplashSpeed = speed;
    {  // the body pushing through the surface sets the water rocking around it
        const float k = clampf(speed / 250.0f, 0.2f, 2.0f) * rippleScale;
        waterField.impulse(centre.x, centre.z, 0.22f * k, 8.0f + 4.0f * k, 0.30f * k);
    }
    const Vec3 move = hspeed > 1.0f ? Vec3(vel.x, 0, vel.z) / hspeed : Vec3(0, 0, 0);
    const float hw = halfWidth;
    // Share of the displaced water that leaves as flying droplets (a crown of splash) - none when wading slowly.
    const float dropletShare = clampf((speed - 70.0f) / 350.0f, 0.0f, 0.7f);
    int dropletBudget = std::min((int)std::min(500.0f, 20.0f + speed * 0.9f), std::max(0, 1200 - (int)particles.size()));  // keeps the air from filling up
    const float savedScale = splashScale_;
    splashScale_ = clampf(speed / 250.0f, 0.6f, 1.6f);
    placeCeiling_ = topY + 1 + 28;  // a little above the old water surface (the pool rises as it is displaced)
    // A big displaced volume (plunging into a deep pool) is spread over a wider area so it does not heap up.
    const float volumeSpread = 1.0f + std::sqrt((float)cells.size()) / 45.0f;
    for (const WaterCell& c : cells) {
        Vec3 from(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f);
        Vec3 out(from.x - centre.x, 0, from.z - centre.z);
        out = lengthSq(out) > 1e-4f ? normalize(out) : Vec3(rng.range(-1, 1), 0, rng.range(-1, 1));
        // Water that was inside a rigid body starts its flight just outside it (a grain inside the stone would land at once).
        for (int step = 0; step < 80 && world.overlayAt(floorToInt(from.x), floorToInt(from.y), floorToInt(from.z)); step++) from += out;
        // Bow wave: pushed outwards and a bit forwards, with some scatter.
        const Vec3 dir = normalize(out * 0.9f + move * 0.5f + Vec3(rng.range(-0.3f, 0.3f), 0, rng.range(-0.3f, 0.3f)));
        const float edge = std::max(0.0f, hw - std::max(std::fabs(from.x - centre.x), std::fabs(from.z - centre.z)));
        const float range = edge + 2.0f + rng.range(1.0f, 7.0f) * (1.0f + speed / 220.0f) * volumeSpread;
        const bool droplet = c.surface && dropletBudget > 0 && rng.f() < dropletShare;
        if (droplet) {
            dropletBudget--;
            // The crown rises from the water surface next to the body, not from the depth the water was pushed out of.
            const int top = columnSurface(floorToInt(from.x), floorToInt(from.z), std::min(world.SY - 1, topY + 40));
            if ((float)top + 0.5f > from.y) from.y = (float)top + 0.5f;
        }
        launchGrain(from, dir, droplet ? range + rng.range(6.0f, 22.0f) * (speed / 250.0f) : range, droplet, M_WATER, droplet ? speed * 0.45f : 0.0f);
    }
    placeCeiling_ = -1;
    splashScale_ = savedScale;
}

// Rigid bodies are solid to water too (their overlay cells block it): whatever water is inside a moving body's cells
// was just pushed in by the body, so it is thrown out the same way the player's displaced water is - the faster the
// body hits the surface, the bigger the splash. Stone stays inert for sand-style effects; it only throws water.
void Game::updateBodySplashes() {
    if (world.counts[M_WATER] == 0) return;
    static std::vector<WaterCell> evicted;
    for (auto& up : physics.bodies) {
        Body& b = *up;
        if (b.dead || b.asleep || b.ovCells.empty()) continue;
        evicted.clear();
        int bx0 = 1 << 30, by0 = 1 << 30, bz0 = 1 << 30, bx1 = -1, by1 = -1, bz1 = -1;
        for (uint32_t i : b.ovCells) {
            if ((world.data[i] & VOX_MASK) != M_WATER) continue;
            int x, y, z;
            world.cellOfIndex(i, x, y, z);
            world.setRaw(x, y, z, M_AIR);
            evicted.push_back({x, y, z, false});
            bx0 = std::min(bx0, x); by0 = std::min(by0, y); bz0 = std::min(bz0, z);
            bx1 = std::max(bx1, x); by1 = std::max(by1, y); bz1 = std::max(bz1, z);
            if (evicted.size() >= 20000) break;
        }
        if (evicted.empty()) continue;
        world.notifyBoxWater(bx0, by0, bz0, bx1, by1, bz1);
        // The top few voxels of each column are the ones that splash.
        std::sort(evicted.begin(), evicted.end(), [](const WaterCell& a, const WaterCell& c) {
            return a.z != c.z ? a.z < c.z : (a.x != c.x ? a.x < c.x : a.y < c.y);
        });
        for (size_t k = 0; k < evicted.size(); k++) {
            int above = 0;  // evicted cells higher up in the same column (the list is sorted by z, x, y)
            for (size_t j = k + 1; j < evicted.size() && evicted[j].x == evicted[k].x && evicted[j].z == evicted[k].z && above < 3; j++) above++;
            evicted[k].surface = above < 3;
        }
        const float vol = clampf(std::sqrt((float)b.count) / 14.0f, 0.5f, 2.5f);
        throwWater(evicted, b.pos, b.vel, std::max(2.0f, b.radius * 0.7f), by1, vol);
    }
}
