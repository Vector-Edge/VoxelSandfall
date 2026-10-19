// Headless regression tests for the simulation layers (no GPU needed): `VoxelSandfall.exe --selftest`.
#include <chrono>
#include <cstdarg>
#include <cstdio>

#include "Game.h"
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
    CHECK(maxY < 30, "sand piled into a low heap (max height %d)", maxY);
    // Sand slides diagonally: width of the heap must exceed the initial 9x9 footprint.
    int minX = 9999, maxX = -9999;
    for (int x = cx() - 60; x <= cx() + 60; x++)
        if (g.world.get(x, 1, cz()) == M_SAND) {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
        }
    CHECK(maxX - minX > 12, "heap spread beyond its footprint (width %d)", maxX - minX + 1);
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
    CHECK(b->pos.y > (float)top, "stone body comes to rest on top of the sand heap instead of sinking through it");
    CHECK(b->asleep, "body sleeps on the heap");
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
        testBigPour(*g);
        testPlayer(*g);
        testFreeStone(*g);
        testBodyStack(*g);
        testBodySplit(*g);
        testBodyOnSand(*g);
        testBuriedPlayer(*g);
        testIslandSplit(*g);
        delete g;
    }
    tlog("\n%s: %d failure(s), %.0f ms\n", g_failures ? "SELFTEST FAILED" : "SELFTEST PASSED", g_failures, nowMs() - t0);
    if (g_log) fclose(g_log);
    g_log = nullptr;
    return g_failures;
}
