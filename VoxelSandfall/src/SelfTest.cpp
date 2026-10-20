// Headless regression tests for the simulation layers (no GPU needed): `VoxelSandfall.exe --selftest`.
#include <chrono>
#include <cstdarg>
#include <cstdio>

#include "Game.h"
#include "Mesher.h"
#include "Parallel.h"

namespace {

FILE* g_log = nullptr;
int g_failures = 0;

void tlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log) {
        vfprintf(g_log, fmt, ap);
        fflush(g_log);
    }
    va_end(ap);
}

#define CHECK(cond, ...)                                      \
    do {                                                      \
        if (cond) {                                           \
            tlog("  [ ok ] " __VA_ARGS__);                    \
            tlog("\n");                                       \
        } else {                                              \
            tlog("  [FAIL] " __VA_ARGS__);                    \
            tlog("   (%s:%d)\n", __FILE__, __LINE__);         \
            g_failures++;                                     \
        }                                                     \
    } while (0)

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void runSim(Game& g, int ticks) {
    for (int i = 0; i < ticks; i++) g.world.simTick(g.rng);
}

// Run the automaton until nothing has moved for 150 consecutive ticks; returns the ticks used.
int settle(Game& g, int maxTicks) {
    int quiet = 0, ticks = 0;
    while (ticks < maxTicks && quiet < 150) {
        g.world.simTick(g.rng);
        ticks++;
        quiet = g.world.lastTickMoves == 0 ? quiet + 1 : 0;
    }
    return quiet >= 150 ? ticks : -ticks;
}

void runPhysics(Game& g, float seconds) {
    const float dt = 1.0f / cfg::PHYS_HZ;
    int n = (int)(seconds / dt);
    for (int i = 0; i < n; i++) {
        g.physics.step(dt);
        if ((i & 7) == 0) g.physics.rasterizeOverlay();
    }
    g.physics.rasterizeOverlay();
}

int cx() { return cfg::ARENA_OX + cfg::ARENA_TOTAL / 2; }
int cz() { return cfg::ARENA_OZ + cfg::ARENA_TOTAL / 2; }

void testArena(Game& g) {
    tlog("[arena template]\n");
    g.reset();
    const long long bedrock = g.world.counts[M_BEDROCK], stone = g.world.counts[M_STONE];
    CHECK(bedrock == 256LL * 256LL, "bedrock floor = 256x256x1 (%lld)", bedrock);
    const long long ring = (long long)cfg::ARENA_TOTAL * cfg::ARENA_TOTAL - 256LL * 256LL;
    CHECK(stone == ring * cfg::WALL_HEIGHT, "perimeter walls of normal stone, 3 thick (%lld voxels)", stone);
    CHECK(g.world.get(cfg::ARENA_OX + 1, 10, cx()) == M_STONE && g.world.get(cfg::ARENA_OX + 3, 10, cx()) == M_AIR,
          "west wall is exactly 3 voxels thick");
    CHECK(g.world.get(cx(), 0, cz()) == M_BEDROCK && g.world.get(cx(), 1, cz()) == M_AIR, "floor is a single layer");
}

void testSand(Game& g) {
    tlog("[sand cellular automaton]\n");
    g.reset();
    int placed = 0;
    for (int y = 30; y < 50; y++)
        for (int z = -4; z <= 4; z++)
            for (int x = -4; x <= 4; x++) {
                g.world.set(cx() + x, y, cz() + z, M_SAND);
                placed++;
            }
    double t0 = nowMs();
    int ticks = 0;
    while (ticks < 6000) {
        g.world.simTick(g.rng);
        ticks++;
        if (g.world.activeCount() == 0) break;
    }
    double t1 = nowMs();
    CHECK(g.world.activeCount() == 0, "sand settles (%d ticks, %.1f ms)", ticks, t1 - t0);
    CHECK(g.world.counts[M_SAND] == placed, "sand conserved (%lld/%d)", g.world.counts[M_SAND], placed);
    // Every sand voxel must be supported: solid below, or at rest on a pile via diagonal.
    int floating = 0, maxY = 0;
    for (int z = cz() - 40; z <= cz() + 40; z++)
        for (int y = 1; y < 80; y++)
            for (int x = cx() - 40; x <= cx() + 40; x++)
                if (g.world.get(x, y, z) == M_SAND) {
                    maxY = std::max(maxY, y);
                    if (g.world.get(x, y - 1, z) == M_AIR) floating++;
                }
    CHECK(floating == 0, "no sand left floating in mid air (%d)", floating);
    // Angle of repose of the heap: base radius from the footprint area, height from the tallest grain.
    int footprint = 0;
    for (int z = cz() - 60; z <= cz() + 60; z++)
        for (int x = cx() - 60; x <= cx() + 60; x++) footprint += g.world.get(x, 1, z) == M_SAND;
    const double rEq = std::sqrt((double)footprint / 3.14159265);
    const double angle = std::atan2((double)maxY, rEq) * 180.0 / 3.14159265;
    tlog("  heap: footprint %d cells (r=%.1f), height %d -> repose angle %.1f deg\n", footprint, rEq, maxY, angle);
    CHECK(angle > 38.0 && angle < 75.0, "sand piles into a steep, clumpy heap (%.0f degrees)", angle);
    CHECK(footprint > 81, "heap spread beyond its 9x9 footprint (%d cells)", footprint);
}

void testWater(Game& g) {
    tlog("[water cellular automaton]\n");
    g.reset();
    int placed = 0;
    g.world.forEachInSphere(Vec3((float)cx(), 20.0f, (float)cz()), 6.0f, [&](int x, int y, int z) {
        g.world.set(x, y, z, M_WATER);
        placed++;
    });
    runSim(g, 6000);  // 33 simulated seconds; surface water never fully stops diffusing
    CHECK(g.world.counts[M_WATER] == placed, "water conserved (%lld/%d)", g.world.counts[M_WATER], placed);
    int minX = 9999, maxX = -9999, layers = 0, unsupported = 0;
    for (int y = 1; y < 40; y++) {
        bool any = false;
        for (int z = cz() - 80; z <= cz() + 80; z++)
            for (int x = cx() - 80; x <= cx() + 80; x++)
                if (g.world.get(x, y, z) == M_WATER) {
                    any = true;
                    if (y == 1) { minX = std::min(minX, x); maxX = std::max(maxX, x); }
                    if (g.world.get(x, y - 1, z) == M_AIR) unsupported++;
                }
        layers += any;
    }
    for (int y = 1; y <= 6; y++) {
        int n = 0;
        for (int z = cz() - 80; z <= cz() + 80; z++)
            for (int x = cx() - 80; x <= cx() + 80; x++) n += g.world.get(x, y, z) == M_WATER;
        tlog("    water layer y=%d: %d voxels\n", y, n);
    }
    int layer1 = 0;
    for (int z = cz() - 80; z <= cz() + 80; z++)
        for (int x = cx() - 80; x <= cx() + 80; x++) layer1 += g.world.get(x, 1, z) == M_WATER;
    CHECK(layer1 * 10 >= placed * 8, "water flattened into a sheet (%d of %d voxels on the floor, %d layers)", layer1, placed, layers);
    CHECK(maxX - minX > 14, "water spread laterally across the floor (width %d)", maxX - minX + 1);
    CHECK(unsupported < 6, "water does not hang in mid air (%d in transit)", unsupported);
}

// Runs the whole game loop (player, sim, particles) for `seconds` with a constant input.
void runGame(Game& g, float seconds, const GameInput& in) {
    const int n = (int)(seconds * 60.0f);
    for (int i = 0; i < n; i++) g.update(1.0f / 60.0f, in);
}

void fillSand(Game& g, int x0, int x1, int y0, int y1, int z0, int z1) {
    for (int z = z0; z <= z1; z++)
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) g.world.setRaw(x, y, z, M_SAND);
    g.world.notifyBox(x0, y0, z0, x1, y1, z1);
}

void testSandWading(Game& g) {
    tlog("[player wades through a sand pile]\n");
    g.reset();
    fillSand(g, cx() - 90, cx() + 90, 1, 12, cz() - 40, cz() + 10);
    settle(g, 20000);
    const long long sand0 = g.world.counts[M_SAND];
    g.player.pos = Vec3((float)cx(), 1.0f, (float)cz() + 70.0f);
    g.player.vel = Vec3(0, 0, 0);
    g.player.yaw = 0;  // facing -Z, straight into the pile
    GameInput in;
    in.forward = 1;
    in.sprint = true;
    runGame(g, 5.0f, in);
    tlog("  player z=%.1f y=%.1f (pile spanned z %d..%d)\n", g.player.pos.z, g.player.pos.y, cz() - 40, cz() + 10);
    CHECK(g.player.pos.z < (float)(cz() - 25), "the player walks right through the pile instead of being stopped (z=%.1f)", g.player.pos.z);
    runGame(g, 4.0f, GameInput());
    CHECK(g.grainsInFlight() == 0, "all shoved grains have landed (%d still flying)", g.grainsInFlight());
    const long long sand1 = g.world.counts[M_SAND];
    CHECK(sand1 > sand0 * 95 / 100 && sand1 <= sand0, "sand is pushed aside, not destroyed (%lld -> %lld voxels)", sand0, sand1);
    CHECK(g.player.pos.y < 20.0f, "the player ends up on the ground, not buried or launched (y=%.1f)", g.player.pos.y);
}

void testSandFootsteps(Game& g) {
    tlog("[footsteps in sand]\n");
    g.reset();
    fillSand(g, cx() - 100, cx() + 100, 1, 5, cz() - 120, cz() + 120);
    settle(g, 20000);
    const long long sand0 = g.world.counts[M_SAND];
    g.player.pos = Vec3((float)cx(), 6.0f, (float)cz() + 100.0f);
    g.player.vel = Vec3(0, 0, 0);
    g.player.yaw = 0;
    GameInput in;
    in.forward = 1;
    runGame(g, 3.0f, in);
    runGame(g, 4.0f, GameInput());
    int dents = 0, scattered = 0;
    for (int z = cz() - 120; z <= cz() + 120; z++)
        for (int x = cx() - 30; x <= cx() + 30; x++) {
            dents += (g.world.get(x, 5, z) == M_AIR);   // a hole in the old surface
            scattered += (g.world.get(x, 6, z) == M_SAND);  // grains lying on top of it
        }
    tlog("  %d dented surface cells, %d kicked grains lying on the surface\n", dents, scattered);
    CHECK(dents > 40, "walking leaves footprints in the sand (%d dented cells)", dents);
    CHECK(scattered > 20, "kicked-up grains land back on the surface (%d)", scattered);
    const long long sand1 = g.world.counts[M_SAND];
    CHECK(sand1 < sand0 && sand1 > sand0 - 6000, "only a little sand is compacted away (%lld -> %lld)", sand0, sand1);
}

void testSandCrater(Game& g) {
    tlog("[landing crater]\n");
    g.reset();
    fillSand(g, cx() - 80, cx() + 80, 1, 6, cz() - 80, cz() + 80);
    settle(g, 20000);
    const long long sand0 = g.world.counts[M_SAND];
    g.player.pos = Vec3((float)cx(), 130.0f, (float)cz());
    g.player.vel = Vec3(0, 0, 0);
    runGame(g, 3.5f, GameInput());
    int dents = 0;
    for (int z = cz() - 20; z <= cz() + 20; z++)
        for (int x = cx() - 20; x <= cx() + 20; x++) dents += (g.world.get(x, 6, z) == M_AIR);
    tlog("  crater: %d cells missing from the top layer, sand %lld -> %lld\n", dents, sand0, g.world.counts[M_SAND]);
    CHECK(dents > 120, "a hard landing digs a crater (%d cells)", dents);
    CHECK(g.player.onGround && g.player.pos.y < 20.0f, "and the player stands in it afterwards (y=%.1f)", g.player.pos.y);
    CHECK(g.world.counts[M_SAND] > sand0 - 1500, "most of the sand is thrown out onto the rim (%lld left)", g.world.counts[M_SAND]);
}

// A walled basin: returns the interior bounds.
struct Basin { int x0, x1, z0, z1; };
Basin buildBasin(Game& g, int half, int wallH) {
    Basin b{cx() - half, cx() + half - 1, cz() - half, cz() + half - 1};
    for (int z = b.z0 - 3; z <= b.z1 + 3; z++)
        for (int x = b.x0 - 3; x <= b.x1 + 3; x++) {
            const bool inside = x >= b.x0 && x <= b.x1 && z >= b.z0 && z <= b.z1;
            if (inside) continue;
            for (int y = 1; y <= wallH; y++) g.world.setRaw(x, y, z, M_STONE);
        }
    g.world.notifyBox(b.x0 - 3, 0, b.z0 - 3, b.x1 + 3, wallH, b.z1 + 3);
    return b;
}

// Fraction of columns whose surface is within the two integer levels that bracket the mean depth.
double flatFraction(Game& g, int x0, int x1, int z0, int z1, long long volume, int* lo, int* hi) {
    const int cols = (x1 - x0 + 1) * (z1 - z0 + 1);
    const double mean = (double)volume / (double)cols;
    const int fl = (int)std::floor(mean + 1e-9), ce = (int)std::ceil(mean - 1e-9);
    int good = 0;
    *lo = 1 << 30;
    *hi = -1;
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++) {
            int h = 0;
            for (int y = 1; y < 60; y++)
                if (g.world.get(x, y, z) == M_WATER) h = y;
            *lo = std::min(*lo, h);
            *hi = std::max(*hi, h);
            good += (h >= fl && h <= ce);
        }
    return (double)good / (double)cols;
}

void testWaterLevel(Game& g) {
    tlog("[water finds its level]\n");
    g.reset();
    const Basin b = buildBasin(g, 30, 45);
    long long placed = 0;
    for (int z = b.z0; z < b.z0 + 20; z++)
        for (int y = 1; y <= 40; y++)
            for (int x = b.x0; x < b.x0 + 20; x++) {
                g.world.setRaw(x, y, z, M_WATER);
                placed++;
            }
    g.world.notifyBox(b.x0, 1, b.z0, b.x0 + 19, 40, b.z0 + 19);
    double t0 = nowMs();
    int ticks = settle(g, 30000);
    double t1 = nowMs();
    int lo, hi;
    const double flat = flatFraction(g, b.x0, b.x1, b.z0, b.z1, placed, &lo, &hi);
    tlog("  %lld voxels poured into one corner of a 60x60 basin: settled after %d ticks (%.0f ms), surface heights %d..%d, %.1f%% of columns at the mean level\n", placed, ticks, t1 - t0, lo, hi, flat * 100.0);
    CHECK(ticks > 0, "the pool comes to rest");
    CHECK(g.world.counts[M_WATER] == placed, "no water lost (%lld/%lld)", g.world.counts[M_WATER], placed);
    CHECK(flat > 0.97 && hi - lo <= 2, "the whole basin is flat (%.1f%% of columns at the mean level, heights %d..%d)", flat * 100.0, lo, hi);
}

void testWaterLevelWide(Game& g) {
    tlog("[water finds its level across a wide floor]\n");
    g.reset();
    // A raised stone ring around a 120 x 24 trench; pour water at one end.
    for (int z = cz() - 12 - 3; z <= cz() + 11 + 3; z++)
        for (int x = cx() - 60 - 3; x <= cx() + 59 + 3; x++) {
            const bool inside = x >= cx() - 60 && x <= cx() + 59 && z >= cz() - 12 && z <= cz() + 11;
            if (inside) continue;
            for (int y = 1; y <= 30; y++) g.world.setRaw(x, y, z, M_STONE);
        }
    long long placed = 0;
    for (int z = cz() - 12; z <= cz() + 11; z++)
        for (int y = 1; y <= 24; y++)
            for (int x = cx() - 60; x < cx() - 40; x++) {
                g.world.setRaw(x, y, z, M_WATER);
                placed++;
            }
    g.world.notifyBox(cx() - 63, 0, cz() - 15, cx() + 62, 30, cz() + 14);
    settle(g, 40000);
    int lo, hi;
    const double flat = flatFraction(g, cx() - 60, cx() + 59, cz() - 12, cz() + 11, placed, &lo, &hi);
    tlog("  %lld voxels at one end of a 120x24 trench -> heights %d..%d, %.1f%% of columns at the mean level\n", placed, lo, hi, flat * 100.0);
    CHECK(flat > 0.92 && hi - lo <= 2, "water has flowed the full length and is (nearly) flat (%.1f%% at the mean level, heights %d..%d)", flat * 100.0, lo, hi);
}

// Water voxels currently inside the player's box (the game keeps this at zero).
int waterInsideBox(Game& g) {
    const float hw = cfg::PLAYER_CORE * 0.5f;
    const Vec3 mn(g.player.pos.x - hw, g.player.pos.y, g.player.pos.z - hw), mx(g.player.pos.x + hw, g.player.pos.y + cfg::PLAYER_H, g.player.pos.z + hw);
    int n = 0;
    for (int z = floorToInt(mn.z + 0.01f); z <= (int)std::ceil(mx.z - 0.01f) - 1; z++)
        for (int y = floorToInt(mn.y + 0.01f); y <= std::min((int)std::ceil(mx.y - 0.01f) - 1, floorToInt(mn.y) + 40); y++)
            for (int x = floorToInt(mn.x + 0.01f); x <= (int)std::ceil(mx.x - 0.01f) - 1; x++) n += g.world.get(x, y, z) == M_WATER;
    return n;
}

void fillWater(Game& g, const Basin& b, int depth) {
    for (int z = b.z0; z <= b.z1; z++)
        for (int y = 1; y <= depth; y++)
            for (int x = b.x0; x <= b.x1; x++) g.world.setRaw(x, y, z, M_WATER);
    g.world.notifyBox(b.x0, 1, b.z0, b.x1, depth, b.z1);
}

// Highest water voxel that is connected to the pool floor by an unbroken column of water, over a footprint.
int poolMoundTop(Game& g, const Basin& b) {
    int top = 0;
    for (int z = b.z0; z <= b.z1; z++)
        for (int x = b.x0; x <= b.x1; x++) {
            int y = 1;
            while (y < g.world.SY && g.world.get(x, y, z) == M_WATER) y++;
            top = std::max(top, y - 1);
        }
    return top;
}

void testWaterImpact(Game& g) {
    tlog("[falling water splashes into a pool instead of piling on top]\n");
    g.reset();
    const Basin b = buildBasin(g, 60, 90);
    const int level = 12;
    fillWater(g, b, level);
    settle(g, 5000);
    long long blob = 0;
    g.world.forEachInSphere(Vec3((float)cx(), 60.0f, (float)cz()), 9.0f, [&](int x, int y, int z) {
        g.world.setRaw(x, y, z, M_WATER);
        blob++;
    });
    g.world.notifyBox(cx() - 10, 48, cz() - 10, cx() + 10, 72, cz() + 10);
    const long long water0 = g.world.counts[M_WATER];
    int peakMound = 0, events = 0, heapTicks = 0;  // heapTicks: ticks the blob stays heaped more than 6 voxels above the pool
    g.world.splashes.clear();
    for (int t = 0; t < 300; t++) {
        g.world.simTick(g.rng);
        events += (int)g.world.splashes.size();
        g.world.splashes.clear();
        const int mound = poolMoundTop(g, b) - level;
        peakMound = std::max(peakMound, mound);
        heapTicks += mound > 6;
    }
    tlog("  a %lld voxel blob dropped from 48 voxels onto a pool %d deep: peak %d voxels above the pool on arrival, heaped (>6) for %d ticks, %d splash events\n",
         blob, level, peakMound, heapTicks, events);
    CHECK(g.world.counts[M_WATER] == water0, "no water created or lost (%lld -> %lld)", water0, g.world.counts[M_WATER]);
    // Without soaking, the blob sits on the surface and slowly spreads (about 35 ticks above 6 voxels).
    CHECK(heapTicks <= 12, "the blob soaks into the pool instead of lying on it like slime (%d ticks heaped)", heapTicks);
    CHECK(events > 0, "the impact is reported as a splash (%d events)", events);
    settle(g, 20000);
    int lo, hi;
    const double flat = flatFraction(g, b.x0, b.x1, b.z0, b.z1, g.world.counts[M_WATER], &lo, &hi);
    CHECK(flat > 0.9 && hi - lo <= 3, "and the pool ends up level (%.1f%% of columns at the mean, heights %d..%d)", flat * 100.0, lo, hi);
}

// Per-column open-water surface heights, built the way the renderer does it (from the water meshes of every chunk).
std::vector<uint16_t> buildWaterTopMap(Game& g) {
    World& w = g.world;
    std::vector<uint16_t> map((size_t)WaterField::W * WaterField::D, 0);
    static MeshData opaque, water;
    uint8_t tops[1024];
    for (int cz = 0; cz < w.CZ; cz++)
        for (int cy = 0; cy < w.CY; cy++)
            for (int cx = 0; cx < w.CX; cx++) {
                buildChunkMesh(w, cx, cy, cz, opaque, water);
                waterTopsOfMesh(water, cx, cy, cz, tops);
                for (int lz = 0; lz < 32; lz++)
                    for (int lx = 0; lx < 32; lx++) {
                        const uint8_t t = tops[lx + 32 * lz];
                        if (!t) continue;
                        uint16_t& m = map[(size_t)(cz * 32 + lz) * WaterField::W + (size_t)(cx * 32 + lx)];
                        m = std::max<uint16_t>(m, (uint16_t)(cy * 32 + t));
                    }
            }
    return map;
}

void testWaterField(Game& g) {
    tlog("[water surface field: ripples, foam, smooth surface]\n");
    g.reset();
    const Basin b = buildBasin(g, 40, 40);
    fillWater(g, b, 10);
    settle(g, 5000);
    const std::vector<uint16_t> map = buildWaterTopMap(g);
    int wrong = 0, wet = 0;
    for (int z = 0; z < WaterField::D; z++)
        for (int x = 0; x < WaterField::W; x++) {
            const bool inside = x >= b.x0 && x <= b.x1 && z >= b.z0 && z <= b.z1;
            const uint16_t t = map[(size_t)z * WaterField::W + (size_t)x];
            wet += t != 0;
            wrong += inside ? (t != 11) : (t != 0);
        }
    tlog("  surface map: %d wet columns, %d wrong\n", wet, wrong);
    CHECK(wrong == 0 && wet == (b.x1 - b.x0 + 1) * (b.z1 - b.z0 + 1), "the renderer's surface map matches the pool exactly (%d wrong columns)", wrong);

    WaterField f;
    f.setSurface(map.data());
    CHECK(f.hasWater() && f.isWater(cx(), cz()) && !f.isWater(b.x0 - 5, cz()), "the field knows where the water is");
    f.impulse((float)cx(), (float)cz(), 0.5f, 3.0f, 0.5f);
    CHECK(f.rippleAt(cx(), cz()) < -0.2f && f.foamAt(cx(), cz()) > 0.2f, "a drop presses the surface down and leaves foam (%.2f, foam %.2f)", f.rippleAt(cx(), cz()), f.foamAt(cx(), cz()));
    f.impulse((float)(b.x0 - 10), (float)cz(), 0.5f, 3.0f, 0.5f);
    CHECK(f.rippleAt(b.x0 - 10, cz()) == 0.0f, "drops outside the water do nothing");
    float ringAt = 0;
    for (int i = 0; i < 60; i++) {
        f.step(1.0f / 60.0f);
        if (i == 59) {
            for (int d = 3; d < 30; d++) ringAt = std::max(ringAt, std::fabs(f.rippleAt(cx() + d, cz())));
        }
    }
    CHECK(ringAt > 0.01f, "after a second the ripple has travelled outwards (peak %.3f away from the centre)", ringAt);
    bool finite = true, outside = false;
    for (int i = 0; i < 600; i++) {
        f.step(1.0f / 60.0f);
        const float c = f.rippleAt(cx(), cz());
        finite &= std::isfinite(c) && std::fabs(c) < 3.0f;
    }
    for (int x = 0; x < WaterField::W; x++) outside |= (x < b.x0 || x > b.x1) && f.rippleAt(x, cz()) != 0.0f;
    CHECK(finite, "the wave simulation stays stable");
    CHECK(f.peakRipple() < 0.01f, "ripples die away (%.4f left after 11 s)", f.peakRipple());
    CHECK(!outside, "ripples stay inside the water");
    // The texture: a flat pool has no slope, the rim of the pool counts as shore.
    WaterField calm;
    calm.setSurface(map.data());
    const uint16_t* t = calm.texels();
    CHECK(t[((size_t)cz() * WaterField::W + cx()) * 4 + 0] == 0 && t[((size_t)cz() * WaterField::W + cx()) * 4 + 1] == 0, "a flat pool has no surface slope");
    const uint16_t shoreRim = t[((size_t)cz() * WaterField::W + b.x0) * 4 + 3], shoreMid = t[((size_t)cz() * WaterField::W + cx()) * 4 + 3];
    CHECK(shoreRim > shoreMid, "the rim of the pool counts as shore");
    // A staircase surface (one voxel step) is smoothed into a gentle slope.
    std::vector<uint16_t> stairs = map;
    for (int z = b.z0; z <= b.z1; z++)
        for (int x = cx(); x <= b.x1; x++) stairs[(size_t)z * WaterField::W + (size_t)x] = 12;
    WaterField st;
    st.setSurface(stairs.data());
    const float mid = st.smoothAt(cx() - 1, cz()), hi = st.smoothAt(cx() + 6, cz()), lo = st.smoothAt(cx() - 8, cz());
    CHECK(lo < mid && mid < hi && hi - lo <= 1.001f, "a one-voxel step in the surface is blended over several voxels (%.2f %.2f %.2f)", lo, mid, hi);
    CHECK(st.active() == false && !st.consumeDirty() == false, "uploading it marks the texture dirty");
}

int waterParticlesInFlight(Game& g) {
    int n = 0;
    for (const Particle& p : g.particles) n += (p.grain && p.mat == M_WATER) ? 1 : 0;
    return n;
}

void testWaterSplash(Game& g) {
    tlog("[a falling ball of water: splash, droplets, ripples, nothing lost]\n");
    g.reset();
    g.player.pos = Vec3(20.0f, 1.0f, 20.0f);
    g.player.vel = Vec3(0, 0, 0);
    const Basin b = buildBasin(g, 60, 90);
    fillWater(g, b, 12);
    settle(g, 5000);
    long long blob = 0;
    g.world.forEachInSphere(Vec3((float)cx(), 70.0f, (float)cz()), 10.0f, [&](int x, int y, int z) {
        g.world.setRaw(x, y, z, M_WATER);
        blob++;
    });
    g.world.notifyBox(cx() - 11, 58, cz() - 11, cx() + 11, 82, cz() + 11);
    const long long total = g.world.counts[M_WATER];
    // The renderer normally supplies the water surface; do it here so the ripples have something to run on.
    const std::vector<uint16_t> map = buildWaterTopMap(g);
    g.waterField.setSurface(map.data());
    int maxDroplets = 0, maxSpray = 0;
    float peakRipple = 0, peakFoam = 0;
    for (int i = 0; i < 90; i++) {
        g.update(1.0f / 60.0f, GameInput());
        maxDroplets = std::max(maxDroplets, waterParticlesInFlight(g));
        maxSpray = std::max(maxSpray, (int)g.particles.size() - g.grainsInFlight());
        peakRipple = std::max(peakRipple, g.waterField.peakRipple());
        for (int dx = -14; dx <= 14; dx += 2) peakFoam = std::max(peakFoam, g.waterField.foamAt(cx() + dx, cz()));
    }
    tlog("  %lld voxel ball dropped from 58 voxels: up to %d droplets and %d spray cubes in the air, ripples up to %.2f voxels, foam %.2f\n", blob, maxDroplets, maxSpray, peakRipple, peakFoam);
    CHECK(maxDroplets > 20, "the impact throws up droplets (%d at once)", maxDroplets);
    CHECK(peakRipple > 0.05f && peakFoam > 0.05f, "and it sets the surface rocking and leaves foam (ripple %.2f, foam %.2f)", peakRipple, peakFoam);
    runGame(g, 6.0f, GameInput());
    CHECK(waterParticlesInFlight(g) == 0, "every droplet has landed again (%d in the air)", waterParticlesInFlight(g));
    CHECK(g.world.counts[M_WATER] == total, "no water created or lost (%lld -> %lld, %d droplets could not land)", total, g.world.counts[M_WATER], g.lostGrains);
    CHECK(g.world.splashes.empty(), "the splash events were all handled");
    settle(g, 20000);
    int lo, hi;
    const double flat = flatFraction(g, b.x0, b.x1, b.z0, b.z1, g.world.counts[M_WATER], &lo, &hi);
    CHECK(flat > 0.9 && hi - lo <= 3, "the pool ends up level (%.1f%% of columns at the mean, heights %d..%d)", flat * 100.0, lo, hi);
}

// ---- dirt and grass -------------------------------------------------------------------------------------------
void fillBox(Game& g, uint8_t m, int x0, int x1, int y0, int y1, int z0, int z1) {
    for (int z = z0; z <= z1; z++)
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) g.world.setRaw(x, y, z, m);
    g.world.notifyBox(x0, y0, z0, x1, y1, z1);
}

int countIn(Game& g, uint8_t m, int x0, int x1, int y0, int y1, int z0, int z1) {
    int n = 0;
    for (int z = z0; z <= z1; z++)
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) n += g.world.get(x, y, z) == m;
    return n;
}

int looseDirt(Game& g) {
    int n = 0;
    for (size_t i = 0; i < g.world.data.size(); i++) n += ((g.world.data[i] & VOX_MASK) == M_DIRT && (g.world.data[i] & VOX_LOOSE)) ? 1 : 0;
    return n;
}

void testDirtPile(Game& g) {
    tlog("[dirt: clumpier than sand]\n");
    g.reset();
    // the same lump of sand and of loose dirt dropped side by side
    const int sandX = cx() - 60, dirtX = cx() + 40;
    for (int z = cz() - 10; z < cz() + 10; z++)
        for (int y = 40; y < 70; y++)
            for (int x = 0; x < 20; x++) {
                g.world.setRaw(sandX + x, y, z, M_SAND);
                g.world.setLooseDirt(dirtX + x, y, z);
            }
    g.world.notifyBox(sandX, 40, cz() - 10, sandX + 19, 69, cz() + 9);
    g.world.notifyBox(dirtX, 40, cz() - 10, dirtX + 19, 69, cz() + 9);
    const long long sand0 = g.world.counts[M_SAND], dirt0 = g.world.counts[M_DIRT];
    settle(g, 20000);
    auto footprint = [&](int x0, uint8_t m) {
        int cols = 0;
        for (int z = cz() - 60; z < cz() + 60; z++)
            for (int x = x0 - 40; x < x0 + 60; x++) {
                bool any = false;
                for (int y = 1; y < 90 && !any; y++) any = g.world.get(x, y, z) == m;
                cols += any;
            }
        return cols;
    };
    const int fs = footprint(sandX, M_SAND), fd = footprint(dirtX, M_DIRT);
    tlog("  the same %lld voxels spread over %d columns as sand and %d as dirt\n", sand0, fs, fd);
    CHECK(g.world.counts[M_SAND] == sand0 && g.world.counts[M_DIRT] == dirt0, "nothing lost (sand %lld/%lld, dirt %lld/%lld)", g.world.counts[M_SAND], sand0, g.world.counts[M_DIRT], dirt0);
    CHECK(fd * 10 < fs * 8, "the dirt heap is steeper and narrower than the sand heap (%d vs %d columns)", fd, fs);
    CHECK(looseDirt(g) == 0, "all the dirt has settled (%d loose voxels)", looseDirt(g));
}

void testDirtStructure(Game& g) {
    tlog("[dirt: overhangs hold, necks and cut-off lumps break]\n");
    g.reset();
    // a stone wall to hang things from, and four dirt arms of different strength
    fillBox(g, M_STONE, 60, 63, 1, 60, 40, 200);
    // short thick arm: 10 long, 10 wide, 3 thick, tied to the wall along 10 x 3 faces -> mass 300 / 30 contacts = 10: holds
    fillBox(g, M_DIRT, 64, 73, 30, 32, 50, 59);
    // very long arm: 30 long -> 900 / 30 = 30: breaks
    fillBox(g, M_DIRT, 64, 93, 30, 32, 100, 109);
    // a big block on a 2x2 neck: 15^3 lump + neck, only 4 contact faces
    fillBox(g, M_DIRT, 64, 66, 30, 31, 150, 151);
    fillBox(g, M_DIRT, 67, 81, 25, 39, 143, 157);
    // a lump floating in mid air (no contact at all)
    fillBox(g, M_DIRT, 120, 129, 40, 49, 100, 109);
    const int armA = countIn(g, M_DIRT, 64, 73, 30, 32, 50, 59), armB = countIn(g, M_DIRT, 64, 93, 30, 32, 100, 109);
    const int total0 = (int)g.world.counts[M_DIRT];
    tlog("  arms: short %d voxels, long %d; total dirt %d\n", armA, armB, total0);
    settle(g, 20000);
    const int armA1 = countIn(g, M_DIRT, 64, 73, 30, 32, 50, 59), armB1 = countIn(g, M_DIRT, 64, 93, 30, 32, 100, 109);
    const int blob1 = countIn(g, M_DIRT, 64, 81, 25, 39, 143, 157), air1 = countIn(g, M_DIRT, 120, 129, 40, 49, 100, 109);
    tlog("  after: short arm %d/%d, long arm %d/%d, lump on the neck %d, floating lump %d\n", armA1, armA, armB1, armB, blob1, air1);
    CHECK(armA1 == armA, "a short, thick overhang attached along a wide face holds (%d of %d voxels)", armA1, armA);
    CHECK(armB1 < armB / 10, "a long thin arm cannot carry its own weight and breaks (%d of %d left)", armB1, armB);
    CHECK(blob1 < 100, "a big lump hanging on a thin neck breaks off (%d voxels still up there)", blob1);
    CHECK(air1 == 0, "a lump with nothing holding it falls");
    CHECK(g.world.counts[M_DIRT] == total0, "no dirt is lost when things fall (%lld / %d)", g.world.counts[M_DIRT], total0);
    CHECK(looseDirt(g) == 0, "and it all comes to rest");
    // dirt lying on the ground stays where it is, however steep
    fillBox(g, M_DIRT, 150, 169, 1, 30, 150, 169);
    const int tower0 = countIn(g, M_DIRT, 150, 169, 1, 30, 150, 169);
    settle(g, 5000);
    CHECK(countIn(g, M_DIRT, 150, 169, 1, 30, 150, 169) == tower0, "a dirt tower standing on the ground keeps its shape (a vertical 20 x 20 x 30 block)");
}

void testGrass(Game& g) {
    tlog("[grass: only on dirt, a two voxel skin, spreads]\n");
    g.reset();
    fillBox(g, M_DIRT, 100, 139, 1, 6, 100, 139);     // a dirt slab
    fillBox(g, M_STONE, 150, 159, 1, 6, 100, 109);    // stone and sand beside it
    fillBox(g, M_SAND, 170, 179, 1, 6, 100, 109);
    CHECK(!g.world.paintGrass(155, 6, 105), "grass cannot be put on stone");
    CHECK(!g.world.paintGrass(175, 6, 105), "grass cannot be put on sand");
    CHECK(!g.world.paintGrass(120, 20, 120), "grass cannot be put in the air");
    CHECK(!g.world.paintGrass(120, 3, 120), "grass cannot be put deep inside the dirt (only the 2 voxel skin)");
    CHECK(g.world.paintGrass(120, 6, 120), "grass goes onto the surface of the dirt");
    const long long covered0 = g.world.counts[M_DIRT] + g.world.counts[M_GRASS];
    runSim(g, 12000);  // 100 s of growth
    int bad = 0, topCols = 0, thick = 0, deepGrass = 0;
    for (int z = 100; z <= 139; z++)
        for (int x = 100; x <= 139; x++) {
            const bool inner = x >= 103 && x <= 136 && z >= 103 && z <= 136;  // (the edge columns are also skinned from the side)
            int grass = 0;
            for (int y = 1; y <= 6; y++) {
                const bool gr = g.world.get(x, y, z) == M_GRASS;
                grass += gr;
                if (gr && y < 5 && inner) deepGrass++;
            }
            if (inner) thick = std::max(thick, grass);
            topCols += g.world.get(x, 6, z) == M_GRASS && g.world.get(x, 5, z) == M_GRASS;
            if (g.world.get(x, 6, z) != M_GRASS) bad++;
        }
    tlog("  after 100 s: %d of 1600 columns are fully covered, thickest grass %d voxels, %d uncovered tops\n", topCols, thick, bad);
    CHECK(thick <= 2, "grass is never more than 2 voxels thick (%d)", thick);
    CHECK(deepGrass == 0, "and nothing below that skin turns to grass (%d)", deepGrass);
    CHECK(bad == 0, "it covers all the dirt it can reach (%d tops still bare)", bad);
    CHECK(g.world.counts[M_DIRT] + g.world.counts[M_GRASS] == covered0, "grass replaces dirt voxel for voxel");
    CHECK(countIn(g, M_GRASS, 150, 179, 1, 8, 100, 109) == 0, "none of it jumped onto the stone or the sand");
    // burying grass kills it; grass that falls with its dirt turns back into dirt
    fillBox(g, M_STONE, 118, 122, 7, 12, 118, 122);
    runSim(g, 3000);
    CHECK(g.world.get(120, 6, 120) == M_DIRT && g.world.get(120, 5, 120) == M_DIRT, "grass under a stone dies back to dirt");
}

void addBox(std::vector<IVec3>& cells, int x0, int y0, int z0, int sx, int sy, int sz);

// ---- chunk loading: the infinite world ------------------------------------------------------------------------
// Global voxel -> local (window) coordinates.
int lx(Game& g, long long gx) { return (int)(gx - g.world.originX); }
int lz(Game& g, long long gz) { return (int)(gz - g.world.originZ); }

// Puts the player at a global position (inside the window or not) and lets the game recentre the window.
void walkTo(Game& g, long long gx, long long gz) {
    g.player.pos.x = (float)(gx - g.world.originX);
    g.player.pos.z = (float)(gz - g.world.originZ);
    g.player.vel = Vec3(0, 0, 0);
    g.update(1.0f / 120.0f, GameInput());
}

uint64_t hashRegion(Game& g, long long gx0, long long gz0, int sx, int sy, int sz) {
    uint64_t h = 1469598103934665603ull;
    for (int z = 0; z < sz; z++)
        for (int y = 0; y < sy; y++)
            for (int x = 0; x < sx; x++) {
                h ^= (uint64_t)g.world.get(lx(g, gx0 + x), y, lz(g, gz0 + z));
                h *= 1099511628211ull;
            }
    return h;
}

void testStreamingBasics(Game& g) {
    tlog("[chunk loading: the window]\n");
    g.setInfinite(true);
    const int R = World::REGION, C = World::CENTER;
    CHECK(g.world.streaming && g.infinite, "the infinite world is on");
    long long floorVox = 0;
    for (int z = 0; z < g.world.SZ; z++)
        for (int x = 0; x < g.world.SX; x++) floorVox += g.world.get(x, 0, z) == M_BEDROCK && g.world.get(x, 1, z) == M_AIR;
    CHECK(floorVox == (long long)g.world.SX * g.world.SZ, "the whole %d x %d region window has a floor (%lld voxels)", World::WIN_REGIONS, World::WIN_REGIONS, floorVox);
    CHECK(g.world.counts[M_BEDROCK] == floorVox, "voxel counts are right (%lld)", g.world.counts[M_BEDROCK]);
    CHECK(g.player.pos.x == (float)(cfg::WORLD_X / 2) && g.player.pos.z == (float)(cfg::WORLD_Z / 2), "the player starts in the middle of the window");
    CHECK(lx(g, 0) == R * C && lz(g, 0) == R * C, "global (0, 0) is the corner of the middle region (local %d, %d)", lx(g, 0), lz(g, 0));
    // a build in the middle region
    for (int y = 1; y <= 30; y++)
        for (int z = 8; z < 18; z++)
            for (int x = 8; x < 18; x++) g.world.setRaw(lx(g, x), y, lz(g, z), M_STONE);
    g.world.notifyBox(lx(g, 8), 1, lz(g, 8), lx(g, 17), 30, lz(g, 17));
    const long long stone0 = g.world.counts[M_STONE];
    const long long ox0 = g.world.originX, oz0 = g.world.originZ;
    walkTo(g, 20, 34);   // still inside the middle region (with its margin): nothing moves
    CHECK(g.regionsShifted == 0 && g.world.originX == ox0 && g.world.originZ == oz0, "walking around inside the middle region does not recentre the window");
    walkTo(g, 20, 42);   // 10 voxels into the next region: the window slides by one region
    CHECK(g.regionsShifted == 1 && g.world.originZ == oz0 + R && g.world.originX == ox0, "walking into a neighbouring region slides the window by one (origin %lld, %lld)", g.world.originX, g.world.originZ);
    CHECK(g.playerGlobalX() == 20 && g.playerGlobalZ() == 42, "the player's global position did not change (%lld, %lld)", g.playerGlobalX(), g.playerGlobalZ());
    CHECK(g.world.get(lx(g, 12), 15, lz(g, 12)) == M_STONE && g.world.get(lx(g, 12), 31, lz(g, 12)) == M_AIR, "the tower is at the same global position");
    CHECK(g.world.counts[M_STONE] == stone0, "and nothing was lost (%lld stone)", g.world.counts[M_STONE]);
    const int lastR = World::WIN_REGIONS - 1 - C;  // the window spans regions -C .. lastR in x, -C+1 .. lastR+1 in z now
    CHECK(g.world.regionLoaded(0, lastR + 1) && g.world.regionLoaded(-C, lastR + 1) && !g.world.regionLoaded(0, -C) && !g.world.regionLoaded(lastR + 1, 0),
          "the new row of regions is loaded, the far row behind is gone");
    CHECK(g.world.get(lx(g, 8), 0, lz(g, (lastR + 1) * R + 5)) == M_BEDROCK, "the new regions have their floor");
    // the player stays near the middle however often the window slides: walking on in one direction slides it region after region
    const int shifts1 = g.regionsShifted;
    for (int i = 1; i <= 5; i++) walkTo(g, 20, 42 + R * i);
    CHECK(g.regionsShifted == shifts1 + 5 && g.player.pos.z >= (float)(C * R - 8) && g.player.pos.z < (float)((C + 1) * R + 8), "five more regions walked: five slides, the player is still mid-window (z %.0f)", g.player.pos.z);
    long long floor2 = 0;
    for (int z = 0; z < g.world.SZ; z++)
        for (int x = 0; x < g.world.SX; x++) floor2 += g.world.get(x, 0, z) == M_BEDROCK;
    CHECK(floor2 == (long long)g.world.SX * g.world.SZ && g.world.counts[M_BEDROCK] == floor2, "the floor is still complete and the counts are right (%lld)", floor2);
}

void testStreamingPersistence(Game& g) {
    tlog("[chunk loading: what you build stays]\n");
    g.setInfinite(true);
    // a tower of stone, a pool of water, a heap of dirt and a rigid block, spread over the 3 x 3 regions at global (0..96)
    for (int y = 1; y <= 40; y++)
        for (int z = 20; z < 30; z++)
            for (int x = 20; x < 30; x++) g.world.setRaw(lx(g, x), y, lz(g, z), M_STONE);
    for (int y = 1; y <= 6; y++)  // a basin with a pool in it
        for (int z = 59; z < 91; z++)
            for (int x = 59; x < 91; x++) {
                const bool wall = x == 59 || x == 90 || z == 59 || z == 90;
                g.world.setRaw(lx(g, x), y, lz(g, z), wall ? M_STONE : (y <= 4 ? M_WATER : M_AIR));
            }
    for (int y = 1; y <= 10; y++)
        for (int z = 10; z < 20; z++)
            for (int x = 60; x < 70; x++) g.world.setRaw(lx(g, x), y, lz(g, z), M_DIRT);
    g.world.notifyBox(lx(g, 0), 1, lz(g, 0), lx(g, 95), 45, lz(g, 95));
    std::vector<IVec3> cells;
    addBox(cells, lx(g, 10), 5, lz(g, 70), 6, 6, 6);
    Body* body = g.physics.createFromWorldCells(cells);
    const int bodyId = body->id;
    settle(g, 4000);
    runPhysics(g, 1.0f);
    const uint64_t h0 = hashRegion(g, 0, 0, 96, 60, 96);
    const long long stone0 = g.world.counts[M_STONE], water0 = g.world.counts[M_WATER], dirt0 = g.world.counts[M_DIRT];
    const Vec3 bodyGlobal0 = body->pos + Vec3((float)g.world.originX, 0, (float)g.world.originZ);
    // go far away (the whole window is replaced): the regions are saved and leave the window
    walkTo(g, 500, 16);
    CHECK(g.world.originX == 256 && !g.world.regionLoaded(0, 0) && !g.world.regionLoaded(2, 2), "far away: the starting regions are not loaded any more (origin %lld)", g.world.originX);
    CHECK(g.world.savedRegions.size() >= 4, "they were saved (%zu regions, %.1f KB)", g.world.savedRegions.size(), (double)g.world.savedBytes() / 1024.0);
    CHECK(g.world.counts[M_STONE] == 0 && g.world.counts[M_WATER] == 0 && g.world.counts[M_DIRT] == 0, "their voxels are no longer counted");
    CHECK(g.stashedBodies() == 1 && g.physics.bodies.empty(), "the rigid block is kept aside (%zu)", g.stashedBodies());
    // an unmodified far region is not stored at all
    const size_t saved1 = g.world.savedRegions.size();
    walkTo(g, 32 * 40 + 16, 16);
    CHECK(g.world.savedRegions.size() == saved1, "untouched regions are never stored (they are generated again when needed)");
    // and back again
    walkTo(g, 48, 48);
    CHECK(g.world.regionLoaded(0, 0) && g.world.regionLoaded(2, 2), "back home: the starting regions are loaded again");
    const uint64_t h1 = hashRegion(g, 0, 0, 96, 60, 96);
    CHECK(h0 == h1, "every voxel of them is exactly as it was (%016llx)", (unsigned long long)h1);
    CHECK(g.world.counts[M_STONE] == stone0 && g.world.counts[M_WATER] == water0 && g.world.counts[M_DIRT] == dirt0, "with the same counts (stone %lld, water %lld, dirt %lld)", g.world.counts[M_STONE], g.world.counts[M_WATER], g.world.counts[M_DIRT]);
    CHECK(g.stashedBodies() == 0 && g.physics.bodies.size() == 1 && g.physics.bodies[0]->id == bodyId, "and the rigid block is back");
    if (!g.physics.bodies.empty()) {
        const Body& b = *g.physics.bodies[0];
        const Vec3 gl = b.pos + Vec3((float)g.world.originX, 0, (float)g.world.originZ);
        CHECK(length(gl - bodyGlobal0) < 1.0f, "at the same place (moved %.2f voxels)", length(gl - bodyGlobal0));
    }
}

void testStreamingSimulation(Game& g) {
    tlog("[chunk loading: the simulation carries on across a recentring]\n");
    g.setInfinite(true);
    // water and sand falling east of the start, clear of the player: sliding the window must not freeze or lose any of it
    long long water = 0, sand = 0;
    for (int y = 1; y <= 12; y++)
        for (int z = 0; z < 30; z++)
            for (int x = 60; x < 85; x++) {
                g.world.setRaw(lx(g, x), y + 20, lz(g, z), M_WATER);
                water++;
            }
    for (int y = 1; y <= 12; y++)
        for (int z = 0; z < 20; z++)
            for (int x = 100; x < 120; x++) {
                g.world.setRaw(lx(g, x), y + 15, lz(g, z), M_SAND);
                sand++;
            }
    g.world.notifyBox(lx(g, 55), 1, lz(g, -5), lx(g, 125), 40, lz(g, 35));
    runSim(g, 40);  // in the air, falling
    const int shifts0 = g.regionsShifted;
    walkTo(g, -12, 16);  // slide the window west while it is all in motion
    CHECK(g.regionsShifted == shifts0 + 1, "the window slid while water and sand were falling");
    runGame(g, 4.0f, GameInput());  // (droplets thrown up by the splashes land again)
    settle(g, 20000);
    CHECK(g.world.counts[M_WATER] == water && g.world.counts[M_SAND] == sand, "nothing lost or frozen (water %lld/%lld, sand %lld/%lld)", g.world.counts[M_WATER], water, g.world.counts[M_SAND], sand);
    int floating = 0;
    for (int z = 0; z < g.world.SZ; z++)
        for (int y = 1; y < 50; y++)
            for (int x = 0; x < g.world.SX; x++)
                if (g.world.get(x, y, z) == M_SAND && g.world.get(x, y - 1, z) == M_AIR) floating++;
    CHECK(floating < 20, "everything came to rest (%d suspended sand voxels)", floating);
    // a pool at the very edge of the window is held back by it, and flows on once the next regions are loaded
    g.setInfinite(true);
    const long long edge = g.world.originX + g.world.SX;  // first global x that is not loaded
    long long w2 = 0;
    for (int y = 1; y <= 8; y++)
        for (int z = 60; z < 110; z++)
            for (int x = (int)edge - 22; x < (int)edge; x++) {
                const bool wall = x == (int)edge - 22 || z == 60 || z == 109;
                g.world.setRaw(lx(g, x), y, lz(g, z), wall ? M_STONE : M_WATER);
                if (!wall) w2++;
            }
    g.world.notifyBox(lx(g, edge - 24), 1, lz(g, 58), g.world.SX - 1, 12, lz(g, 111));
    settle(g, 20000);
    int past = 0;
    for (int z = 0; z < g.world.SZ; z++)
        for (int x = 0; x < g.world.SX; x++) past += (g.world.get(x, 1, z) == M_WATER && g.world.originX + x >= edge);
    CHECK(g.world.counts[M_WATER] == w2 && past == 0, "a pool at the edge of the window is held by it (%lld voxels, %d beyond)", g.world.counts[M_WATER], past);
    walkTo(g, edge - 6, 10);  // walk east (clear of the pool): the next regions are loaded
    runGame(g, 2.0f, GameInput());
    settle(g, 20000);
    past = 0;
    for (int z = 0; z < g.world.SZ; z++)
        for (int x = 0; x < g.world.SX; x++) past += (g.world.get(x, 1, z) == M_WATER && g.world.originX + x >= edge);
    CHECK(g.world.counts[M_WATER] == w2 && past > 100, "...and flows on into the new region once it is loaded (%d voxels beyond the old edge, %lld in total)", past, g.world.counts[M_WATER]);
}

void testWadingInWater(Game& g) {
    tlog("[wading through water]\n");
    g.reset();
    const Basin b = buildBasin(g, 80, 70);
    fillWater(g, b, 20);
    settle(g, 5000);
    const long long water0 = g.world.counts[M_WATER];
    g.player.pos = Vec3((float)cx(), 1.0f, (float)(b.z1 - 30));
    g.player.vel = Vec3(0, 0, 0);
    g.player.yaw = 0;
    GameInput in;
    in.forward = 1;
    int leaks = 0, maxGrains = 0;
    float dist0 = g.player.pos.z;
    const int frames = 100;
    float zAtHalf = 0;
    for (int i = 0; i < frames; i++) {
        g.update(1.0f / 60.0f, in);
        if (i >= 4) leaks += waterInsideBox(g);
        maxGrains = std::max(maxGrains, g.grainsInFlight());
        if (i == 59) zAtHalf = g.player.pos.z;
    }
    const float travelled = dist0 - zAtHalf;  // after 1 s
    tlog("  walked %.0f voxels in 1 s (dry walking is ~170), droplets in flight peaked at %d, speed at last splash %.0f\n", travelled, maxGrains, g.lastSplashSpeed);
    CHECK(travelled < 160.0f && travelled > 40.0f, "wading is slower than walking on dry ground (%.0f voxels in 1 s)", travelled);
    CHECK(leaks == 0, "the player never ends up inside the water (%d water voxel-frames inside the body)", leaks);
    // stop and let everything run back: the wake behind the player must have closed up
    runGame(g, 4.0f, GameInput());
    CHECK(g.grainsInFlight() == 0, "all droplets have landed (%d flying)", g.grainsInFlight());
    const long long water1 = g.world.counts[M_WATER];
    CHECK(water1 > water0 * 98 / 100 && water1 <= water0, "water is displaced, not destroyed (%lld -> %lld)", water0, water1);
    int dry = 0, cells = 0;
    const int zStart = b.z1 - 30, zNow = floorToInt(g.player.pos.z);
    for (int z = zNow + 40; z <= zStart; z++)
        for (int x = cx() - 20; x <= cx() + 20; x++) {
            cells++;
            int h = 0;
            for (int y = 1; y < 40; y++) h += g.world.get(x, y, z) == M_WATER;
            dry += h < 18;
        }
    tlog("  the path behind the player: %d of %d columns are shallower than 18 (level is 20)\n", dry, cells);
    CHECK(dry * 20 < cells, "water flows back in behind the player (%d of %d columns still hollow)", dry, cells);
}

void testPlungeIntoWater(Game& g) {
    tlog("[jumping into water]\n");
    g.reset();
    const Basin b = buildBasin(g, 70, 135);
    fillWater(g, b, 90);
    settle(g, 5000);
    const long long water0 = g.world.counts[M_WATER];
    g.player.pos = Vec3((float)cx(), 150.0f, (float)cz());
    g.player.vel = Vec3(0, 0, 0);
    int maxGrains = 0;
    float peakSplash = 0;
    for (int i = 0; i < 90; i++) {
        g.update(1.0f / 60.0f, GameInput());
        maxGrains = std::max(maxGrains, g.grainsInFlight());
        peakSplash = std::max(peakSplash, g.lastSplashSpeed);
    }
    tlog("  splash: %.0f voxels/s impact, %d droplets in the air at once, player at y=%.1f\n", peakSplash, maxGrains, g.player.pos.y);
    CHECK(peakSplash > 200.0f, "falling into water registers as a fast impact (%.0f)", peakSplash);
    CHECK(maxGrains > 100, "a plunge throws up a splash of droplets (%d)", maxGrains);
    runGame(g, 5.0f, GameInput());
    tlog("  after 5 s: player y=%.1f (water surface at 90), waterFrac=%.2f\n", g.player.pos.y, g.player.waterFrac);
    CHECK(g.player.pos.y > 8.0f && g.player.pos.y < 50.0f && g.player.waterFrac > 0.4f, "the player floats with the head above water (feet at y=%.1f)", g.player.pos.y);
    CHECK(waterInsideBox(g) == 0, "and still pushes the water out of his body");
    CHECK(g.grainsInFlight() == 0, "the splash has come back down (%d droplets)", g.grainsInFlight());
    const long long water1 = g.world.counts[M_WATER];
    CHECK(water1 > water0 * 97 / 100, "almost all the displaced water is still there (%lld -> %lld)", water0, water1);
}

void testStoneDoesNotSplash(Game& g) {
    tlog("[stone does not react]\n");
    g.reset();
    g.player.pos = Vec3((float)cx(), 1.0f, (float)cz() + 60.0f);
    GameInput in;
    in.forward = 1;
    in.sprint = true;
    runGame(g, 1.5f, in);
    CHECK(g.particles.empty(), "running on bedrock throws nothing up (%zu particles)", g.particles.size());
    // dropping onto bedrock from height: no crater, no splash
    g.player.pos = Vec3((float)cx(), 200.0f, (float)cz());
    g.player.vel = Vec3(0, 0, 0);
    runGame(g, 3.0f, GameInput());
    CHECK(g.particles.empty(), "landing hard on stone is silent (%zu particles)", g.particles.size());
}

void testBigPour(Game& g) {
    tlog("[parallel cellular sim: 460k voxel pour]\n");
    g.reset();
    const long long sand0 = 0;
    (void)sand0;
    long long sand = 0, water = 0;
    g.world.forEachInSphere(Vec3((float)cx() - 40.0f, 110.0f, (float)cz()), 40.0f, [&](int x, int y, int z) {
        g.world.setRaw(x, y, z, M_SAND);
        sand++;
    });
    g.world.forEachInSphere(Vec3((float)cx() + 40.0f, 110.0f, (float)cz()), 40.0f, [&](int x, int y, int z) {
        g.world.setRaw(x, y, z, M_WATER);
        water++;
    });
    g.world.notifyBox(0, 0, 0, g.world.SX - 1, g.world.SY - 1, g.world.SZ - 1);
    double t0 = nowMs();
    double firstTicks = 0;
    for (int i = 0; i < 20; i++) g.world.simTick(g.rng);
    firstTicks = nowMs() - t0;
    tlog("  first 20 ticks (%lld voxels active): %.1f ms (%.2f ms/tick, %d threads)\n", sand + water, firstTicks, firstTicks / 20.0, globalPool().participants());
    runSim(g, 1500);
    CHECK(g.world.counts[M_SAND] == sand && g.world.counts[M_WATER] == water, "no voxel lost or duplicated by the parallel sim (sand %lld/%lld, water %lld/%lld)",
          g.world.counts[M_SAND], sand, g.world.counts[M_WATER], water);
    // Nothing may be left floating (every sand voxel rests on something) once the pile has settled.
    runSim(g, 3000);
    int floating = 0;
    for (int z = 0; z < g.world.SZ; z++)
        for (int y = 1; y < g.world.SY; y++)
            for (int x = 0; x < g.world.SX; x++)
                if (g.world.get(x, y, z) == M_SAND && g.world.get(x, y - 1, z) == M_AIR) floating++;
    CHECK(floating < 40, "settled sand does not hang in mid air (%d suspended voxels)", floating);
}

void testIslandSplit(Game& g) {
    tlog("[island detection / splitting]\n");
    g.reset();
    const int wz = cfg::ARENA_OZ + 1;  // middle of the north wall's 3-voxel thickness
    const long long stone0 = g.world.counts[M_STONE];
    // Cut a 30x30 patch free of the top of the wall: carve its left, right and bottom edges.
    double t0 = nowMs();
    int islands = 0;
    auto carve = [&](float x, float y) {
        CarveResult r = carveSphere(g.world, g.physics, Vec3(x, y, (float)wz + 0.5f), 3.2f, g.rng);
        islands += r.islandsCreated;
    };
    for (int x = 98; x <= 132; x += 3) carve((float)x, 98.0f);  // bottom edge (y = 98)
    for (int y = 98; y <= 160; y += 3) { carve(98.0f, (float)y); carve(132.0f, (float)y); }
    double t1 = nowMs();
    CHECK(islands >= 1, "carving a patch free creates a rigid body (islands=%d, %.1f ms)", islands, t1 - t0);
    CHECK(g.physics.bodies.size() >= 1, "bodies in the physics world: %zu", g.physics.bodies.size());
    size_t biggest = 0;
    for (auto& b : g.physics.bodies) biggest = std::max(biggest, (size_t)b->count);
    CHECK(biggest > 1500, "largest detached island has %zu voxels", biggest);
    CHECK(g.world.counts[M_STONE] + (long long)[&] { long long n = 0; for (auto& b : g.physics.bodies) n += b->count; return n; }() <= stone0,
          "stone voxels are conserved or destroyed, never duplicated");
    // The rest of the wall must still be anchored (no spurious islands from the intact structure).
    CHECK(g.world.get(cfg::ARENA_OX + 1, 100, cz()) == M_STONE, "unrelated parts of the structure stay static");

    // Let the island fall, tumble and come to rest on whatever is below.
    t0 = nowMs();
    runPhysics(g, 4.0f);
    t1 = nowMs();
    int asleep = 0;
    float minY = 1e9f;
    for (auto& b : g.physics.bodies) {
        asleep += b->asleep;
        minY = std::min(minY, b->pos.y - b->radius);
    }
    tlog("  physics: 4 s simulated in %.0f ms, %zu bodies, %d asleep\n", t1 - t0, g.physics.bodies.size(), asleep);
    CHECK((size_t)asleep == g.physics.bodies.size(), "all bodies come to rest and sleep");

    // Cutting a carved body again must split it into several bodies.
    size_t before = g.physics.bodies.size();
    Body* big = nullptr;
    for (auto& b : g.physics.bodies)
        if (!big || b->count > big->count) big = b.get();
    Vec3 c = big->pos;
    for (int i = -6; i <= 6; i++) {  // slice through the middle
        Vec3 p = c + Vec3(0, (float)i * 3.0f, 0);
        carveSphere(g.world, g.physics, p + Vec3(0, 0, 0), 3.5f, g.rng);
    }
    CHECK(g.physics.bodies.size() >= before, "shooting a body keeps/splits bodies (%zu -> %zu)", before, g.physics.bodies.size());
}

void testFreeStone(Game& g) {
    tlog("[creator: stone in free space]\n");
    g.reset();
    Vec3 pmin(0, 0, 0), pmax(0, 0, 0);
    PaintResult pr = paintSphere(g.world, g.physics, Vec3((float)cx(), 80.0f, (float)cz()), 5.0f, M_STONE, pmin, pmax);
    CHECK(pr.newBodies == 1 && g.physics.bodies.size() == 1, "free-space stone becomes one rigid body (%d voxels)", pr.placed);
    Body& b = *g.physics.bodies[0];
    runPhysics(g, 0.0f);
    double t0 = nowMs();
    runPhysics(g, 3.0f);
    double t1 = nowMs();
    tlog("  physics: 3 s in %.0f ms\n", t1 - t0);
    CHECK(b.asleep, "body sleeps after landing");
    // Resting on the bedrock floor (top face at y = 1): lowest voxel centre sits ~0.5 above it.
    float lowest = 1e9f;
    for (const Vec3& s : b.surf) lowest = std::min(lowest, (b.pos + b.rot.toMat3() * s).y);
    CHECK(lowest > 1.2f && lowest < 1.9f, "rests on the floor without tunnelling (lowest voxel centre y=%.3f)", lowest);

    // Painting onto the floor attaches to the static structure instead.
    pr = paintSphere(g.world, g.physics, Vec3((float)cx() + 40, 2.0f, (float)cz()), 4.0f, M_STONE, pmin, pmax);
    CHECK(pr.staticStone > 0 && pr.newBodies == 0, "stone painted on bedrock is anchored (%d static voxels)", pr.staticStone);
    // Permanent stone is immune to the gun.
    CarveResult cr = carveSphere(g.world, g.physics, Vec3((float)cx() + 100, 0.5f, (float)cz()), 10.0f, g.rng);
    CHECK(cr.removedWorld == 0 && g.world.get(cx() + 100, 0, cz()) == M_BEDROCK, "permanent stone survives the gun");
}

void addBox(std::vector<IVec3>& cells, int x0, int y0, int z0, int sx, int sy, int sz) {
    for (int k = 0; k < sz; k++)
        for (int j = 0; j < sy; j++)
            for (int i = 0; i < sx; i++) cells.emplace_back(x0 + i, y0 + j, z0 + k);
}

void testBodyStack(Game& g) {
    tlog("[rigid bodies: stacking and body-body collision]\n");
    g.reset();
    std::vector<IVec3> a, b;
    addBox(a, cx() - 10, 20, cz() - 10, 20, 4, 20);
    addBox(b, cx() - 8, 60, cz() - 8, 16, 4, 16);
    Body* A = g.physics.createFromWorldCells(a);
    runPhysics(g, 1.5f);
    Body* B = g.physics.createFromWorldCells(b);
    runPhysics(g, 4.0f);
    CHECK(A->asleep && B->asleep, "slab stack comes to rest");
    CHECK(std::fabs(A->pos.y - 3.0f) < 0.6f, "bottom slab rests on the floor (centre y=%.2f)", A->pos.y);
    float dy = B->pos.y - A->pos.y;
    CHECK(dy > 3.4f && dy < 4.7f, "top slab rests on the bottom slab (centre offset %.2f)", dy);
    CHECK(std::fabs(B->pos.x - A->pos.x) < 1.5f && std::fabs(B->pos.z - A->pos.z) < 1.5f, "stack stays aligned");
}

void testBodySplash(Game& g) {
    tlog("[a stone block falling into a pool splashes like the player does]\n");
    g.reset();
    g.player.pos = Vec3(20.0f, 1.0f, 20.0f);
    g.player.vel = Vec3(0, 0, 0);
    const Basin b = buildBasin(g, 60, 90);
    fillWater(g, b, 30);
    settle(g, 5000);
    const std::vector<uint16_t> map = buildWaterTopMap(g);
    g.waterField.setSurface(map.data());
    const long long total = g.world.counts[M_WATER];
    std::vector<IVec3> cells;
    addBox(cells, cx() - 6, 75, cz() - 6, 12, 12, 12);
    Body* body = g.physics.createFromWorldCells(cells);
    int maxDroplets = 0, maxSpray = 0;
    float peakRipple = 0, peakSpeed = 0;
    for (int i = 0; i < 150; i++) {
        g.update(1.0f / 60.0f, GameInput());
        maxDroplets = std::max(maxDroplets, waterParticlesInFlight(g));
        maxSpray = std::max(maxSpray, (int)g.particles.size() - g.grainsInFlight());
        peakRipple = std::max(peakRipple, g.waterField.peakRipple());
        peakSpeed = std::max(peakSpeed, g.lastSplashSpeed);
    }
    tlog("  12^3 stone block dropped from 44 voxels above the water: impact %.0f voxels/s, %d droplets and %d spray cubes in the air, ripples up to %.2f\n", peakSpeed, maxDroplets, maxSpray, peakRipple);
    CHECK(peakSpeed > 150.0f, "the block hits the water fast (%.0f voxels/s)", peakSpeed);
    CHECK(maxDroplets > 20, "and throws up a crown of droplets (%d at once)", maxDroplets);
    CHECK(peakRipple > 0.05f, "and sets the surface rocking (ripple %.2f)", peakRipple);
    runGame(g, 5.0f, GameInput());
    CHECK(waterParticlesInFlight(g) == 0, "the splash comes back down (%d droplets left)", waterParticlesInFlight(g));
    CHECK(g.world.counts[M_WATER] == total, "the displaced water is not lost (%lld -> %lld, %d could not land)", total, g.world.counts[M_WATER], g.lostGrains);
    int inside = 0;
    for (uint32_t i : body->ovCells) inside += (g.world.data[i] & VOX_MASK) == M_WATER ? 1 : 0;
    CHECK(inside == 0, "no water ends up inside the stone (%d voxels)", inside);
    CHECK(body->pos.y < 25.0f, "and the block sinks (centre y=%.1f)", body->pos.y);
}

void testBodyOnSand(Game& g) {
    tlog("[rigid bodies resting on sand]\n");
    g.reset();
    for (int y = 20; y < 50; y++)
        for (int z = -5; z <= 5; z++)
            for (int x = -5; x <= 5; x++) g.world.set(cx() + x, y, cz() + z, M_SAND);
    settle(g, 20000);
    int top = 0;
    for (int y = 1; y < 60; y++)
        if (g.world.get(cx(), y, cz()) == M_SAND) top = y;
    std::vector<IVec3> cells;
    addBox(cells, cx() - 3, 90, cz() - 3, 6, 2, 6);
    Body* b = g.physics.createFromWorldCells(cells);
    runPhysics(g, 3.0f);
    tlog("  heap top y=%d, body centre y=%.2f\n", top, b->pos.y);
    // The slab may slide off the (steep) heap, but it must never be inside the sand.
    int inside = 0;
    const Mat3 R = b->rot.toMat3();
    for (const Vec3& sv : b->surf) {
        Vec3 p = b->pos + R * sv;
        inside += g.world.get(floorToInt(p.x), floorToInt(p.y), floorToInt(p.z)) == M_SAND;
    }
    CHECK(inside * 20 < (int)b->surf.size(), "stone body stays outside the sand (%d of %zu voxels overlap)", inside, b->surf.size());
    CHECK(b->asleep, "body sleeps");
}

void testBuriedPlayer(Game& g) {
    tlog("[player buried by sand]\n");
    g.reset();
    PlayerInput none;
    for (int i = 0; i < 60; i++) g.player.update(g.world, 1.0f / 60.0f, none);
    Vec3 mn, mx;
    g.player.aabb(mn, mx);
    for (int z = (int)mn.z; z < (int)mx.z; z++)
        for (int y = 1; y < 30; y++)
            for (int x = (int)mn.x; x < (int)mx.x; x++) g.world.set(x, y, z, M_SAND);
    settle(g, 20000);
    for (int i = 0; i < 240; i++) g.player.update(g.world, 1.0f / 60.0f, none);
    CHECK(!g.player.frozen && g.player.pos.y > 25.0f, "player is lifted out of the sand (y=%.1f)", g.player.pos.y);
    CHECK(g.player.onGround, "and ends up standing on the surface");
}

void testBodySplit(Game& g) {
    tlog("[rigid bodies: cutting a body apart]\n");
    g.reset();
    std::vector<IVec3> cells;
    const int bx = cx(), by = 60, bz = cz();
    addBox(cells, bx, by, bz, 6, 6, 6);
    addBox(cells, bx + 6, by + 2, bz + 2, 8, 2, 2);
    addBox(cells, bx + 14, by, bz, 6, 6, 6);
    Body* body = g.physics.createFromWorldCells(cells);
    CHECK(body->count == 216 * 2 + 32, "dumbbell has %d voxels", body->count);
    body->ang = Vec3(0, 4, 0);
    CarveResult cr = carveSphere(g.world, g.physics, Vec3(bx + 10.0f, by + 3.0f, bz + 3.0f), 2.6f, g.rng);
    CHECK(g.physics.bodies.size() == 2, "cutting the bar splits it into two bodies (%zu)", g.physics.bodies.size());
    CHECK(cr.removedBody > 0, "gun removed %d body voxels", cr.removedBody);
    runPhysics(g, 4.0f);
    size_t asleep = 0;
    for (auto& p : g.physics.bodies) asleep += p->asleep;
    CHECK(asleep == g.physics.bodies.size(), "both halves fall and sleep");
}

void testPlayer(Game& g) {
    tlog("[player controller]\n");
    g.reset();
    PlayerInput none;
    for (int i = 0; i < 120; i++) g.player.update(g.world, 1.0f / 60.0f, none);
    CHECK(g.player.onGround && std::fabs(g.player.pos.y - 1.0f) < 0.05f, "stands on the bedrock floor (y=%.3f)", g.player.pos.y);
    PlayerInput jump;
    jump.jump = true;
    float maxY = 0;
    for (int i = 0; i < 60; i++) {
        g.player.update(g.world, 1.0f / 60.0f, jump);
        maxY = std::max(maxY, g.player.pos.y);
        jump.jump = false;
    }
    CHECK(maxY > 30.0f && maxY < 120.0f, "can jump (apex y=%.1f)", maxY);
    // Walk into the north wall (-Z): must stop at the inner face.
    g.player.yaw = 0;
    PlayerInput fwd;
    fwd.forward = 1;
    fwd.sprint = true;
    for (int i = 0; i < 60 * 8; i++) g.player.update(g.world, 1.0f / 60.0f, fwd);
    float innerFace = (float)(cfg::ARENA_OZ + cfg::WALL_THICK);
    CHECK(g.player.pos.z - cfg::PLAYER_W * 0.5f >= innerFace - 0.01f, "walls stop the player (z=%.2f, face=%.0f)", g.player.pos.z, innerFace);
    CHECK(g.player.pos.z - cfg::PLAYER_W * 0.5f < innerFace + 0.5f, "player reaches the wall");
    // Climb a small step (sand ledge 4 voxels high).
    g.reset();
    for (int z = cz() - 80; z <= cz() + 80; z++)
        for (int y = 1; y <= 4; y++)
            for (int x = cx() - 10; x <= cx() + 10; x++) g.world.set(x, y, z, M_STONE);
    g.player.pos = Vec3((float)cx(), 1.0f, (float)cz() + 100.0f);
    for (int i = 0; i < 120; i++) g.player.update(g.world, 1.0f / 60.0f, none);
    g.player.yaw = 0;
    PlayerInput walk;
    walk.forward = 1;
    for (int i = 0; i < 60 * 3; i++) g.player.update(g.world, 1.0f / 60.0f, walk);
    CHECK(g.player.pos.y > 4.5f, "auto-steps onto a 4 voxel ledge (y=%.2f)", g.player.pos.y);
}

void testRay(Game& g) {
    tlog("[ray casting]\n");
    g.reset();
    RayHit h = g.world.raycast(Vec3((float)cx(), 50.0f, (float)cz()), Vec3(0, -1, 0), 200.0f, matBit(M_BEDROCK));
    CHECK(h.hit && h.cell.y == 0 && h.normal.y == 1 && std::fabs(h.t - 49.0f) < 0.01f, "downward ray hits the floor top at t=%.3f", h.t);
    h = g.world.raycast(Vec3((float)cx(), 50.0f, (float)cz()), Vec3(0, 0, -1), 400.0f, matBit(M_STONE));
    CHECK(h.hit && h.cell.z == cfg::ARENA_OZ + 2 && h.normal.z == 1, "horizontal ray hits the inner wall face");
}

}  // namespace

int runSelfTest(const char* logPath) {
    g_log = fopen(logPath, "w");
    g_failures = 0;
    double t0 = nowMs();
    {
        Game* g = new Game();
        testArena(*g);
        testRay(*g);
        testSand(*g);
        testWater(*g);
        testWaterLevel(*g);
        testWaterLevelWide(*g);
        testBigPour(*g);
        testPlayer(*g);
        testFreeStone(*g);
        testBodyStack(*g);
        testBodySplit(*g);
        testBodyOnSand(*g);
        testBodySplash(*g);
        testBuriedPlayer(*g);
        testSandWading(*g);
        testSandFootsteps(*g);
        testSandCrater(*g);
        testWaterImpact(*g);
        testWaterField(*g);
        testWaterSplash(*g);
        testStreamingBasics(*g);
        testStreamingPersistence(*g);
        testStreamingSimulation(*g);
        g->setInfinite(false);
        testDirtPile(*g);
        testDirtStructure(*g);
        testGrass(*g);
        testWadingInWater(*g);
        testPlungeIntoWater(*g);
        testStoneDoesNotSplash(*g);
        testIslandSplit(*g);
        delete g;
    }
    tlog("\n%s: %d failure(s), %.0f ms\n", g_failures ? "SELFTEST FAILED" : "SELFTEST PASSED", g_failures, nowMs() - t0);
    if (g_log) fclose(g_log);
    g_log = nullptr;
    return g_failures;
}
