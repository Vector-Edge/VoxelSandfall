// Cellular automata for the particle materials (spec section 1):
//   Sand  - falls straight down; if blocked, slides diagonally (down-left/right/forward/back).
//   Water - falls straight down; if blocked, spreads laterally into adjacent empty cells.
//   Oil   - a thick liquid: it flows like water but slowly (it hops and squeezes sideways only now and then, and only looks
//           a short way for a lower surface), it floats on water (water sinks through it) and sand sinks through it.
// Only voxels on the active lists are visited, so a settled world costs nothing.
//
// Parallelism: the grid is cut into 8x8 voxel columns ("tiles", full height). Tiles of the same
// checkerboard colour are at least one idle tile (8 voxels) apart and ordinary moves only touch cells a few
// voxels away, so the four colours are simulated one after another while all tiles of a colour run
// concurrently on the thread pool. Newly woken cells are collected per worker and merged afterwards.
//
// Water leveling: a water voxel at the top of its column looks along eight lines (axes + diagonals) - straight
// through other water - for a column at least two voxels lower and proposes to hop there. Those long moves cannot be done
// inside the tile-parallel phase, so they are only *proposed* there and applied serially afterwards. The rule
// only moves a voxel when it strictly lowers the surface difference, so a connected pool settles completely
// flat (every column within one voxel of every other), however wide it is.
//
// Water impacts: a water voxel counts the ticks it has been free-falling (2 bits of its raw byte). One that lands
// after falling for two or more ticks is an impact. Impacts are reported as splash events, a share of them is
// taken out of the grid as flying droplets (when the owner has asked for droplets), and a voxel that lands on a
// pool does not stack on top of it like slime: it soaks in, i.e. it is placed on the lowest nearby surface.
#include <chrono>

#include "Parallel.h"
#include "World.h"

namespace {
constexpr int kDX[4] = {1, -1, 0, 0};
constexpr int kDZ[4] = {0, 0, 1, -1};
// Water looks along these 8 lines (the 4 axes and the 4 diagonals) for a lower surface.
constexpr int kLineDX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kLineDZ[8] = {0, 0, 1, -1, 1, -1, 1, -1};
constexpr int kWaterReach = 128;  // how far a surface water voxel looks for a lower surface (through other water)
constexpr int kHopPercent = 55;   // chance per tick that an unstable surface voxel actually hops (flow speed)
constexpr int kMaxHopsPerTick = 40000;
constexpr int kOilReach = 36;         // how far a surface oil voxel looks for a lower surface
constexpr int kOilHopPercent = 16;    // chance per tick that an unstable surface oil voxel hops (water: kHopPercent)
constexpr int kOilMovePercent = 34;   // chance per tick that oil squeezes sideways under pressure
constexpr int kMaxFall = 3;     // cells a free-falling voxel drops per tick
constexpr int kMaxImpactsPerTick = 3000;  // soaks per tick (the serial pass); water beyond that just lands and flows
constexpr size_t kMaxSplashEvents = 12000;
constexpr int kMaxDirtSeedsPerTick = 800;   // hanging dirt checked per tick (the rest waits)
constexpr int kGrassPeriod = 24;            // grass looks around once every this many ticks (5 Hz)
constexpr int kGrassEntriesPerRun = 4000;   // ... for at most this many grass voxels
// Share (of 16) of sand grains that are loose (slide at a drop of 1), medium (2); the rest are sticky (3).
constexpr uint32_t kLooseGrains = 8, kMediumGrains = 6;
}  // namespace

void World::simTick(Rng&) {
    simTickCount++;
    lastTickMoves = 0;

    // Promote the work queued since the last tick.
    int totalTiles = TX * TZ;
    size_t total = 0;
    for (int t = 0; t < totalTiles; t++) {
        tileCur[(size_t)t].v.swap(tileNext[(size_t)t].v);
        tileNext[(size_t)t].v.clear();
        total += tileCur[(size_t)t].v.size();
    }
    if (total == 0) {
        analyzeDirt(kMaxDirtSeedsPerTick);
        if ((simTickCount % kGrassPeriod) == 0) growGrass(kGrassEntriesPerRun);
        return;
    }
    dbgVisits += (long long)total;

    ThreadPool& pool = globalPool();
    if (simWorkers.size() != (size_t)pool.participants()) simWorkers.resize((size_t)pool.participants());
    for (SimWorker& w : simWorkers) w.moves = 0;

    const auto tPar0 = std::chrono::steady_clock::now();
    static std::vector<int> phaseTiles[4];
    for (auto& p : phaseTiles) p.clear();
    for (int tz = 0; tz < TZ; tz++)
        for (int tx = 0; tx < TX; tx++) {
            int t = tx + TX * tz;
            if (!tileCur[(size_t)t].v.empty()) phaseTiles[(tx & 1) | ((tz & 1) << 1)].push_back(t);
        }

    for (int phase = 0; phase < 4; phase++) {
        const std::vector<int>& list = phaseTiles[phase];
        if (list.empty()) continue;
        // Small phases are not worth waking the pool for.
        size_t work = 0;
        for (int t : list) work += tileCur[(size_t)t].v.size();
        if (work < 2048 || list.size() == 1) {
            for (int t : list) simTile(t, simWorkers[0]);
        } else {
            pool.parallelFor((int)list.size(), [&](int k, int worker) { simTile(list[(size_t)k], simWorkers[(size_t)worker]); });
        }
    }

    const auto tImp0 = std::chrono::steady_clock::now();
    const int nImp = applyImpacts(kMaxImpactsPerTick);
    lastTickMoves += nImp;
    const auto tHop0 = std::chrono::steady_clock::now();
    lastTickMoves += applyWaterHops(kMaxHopsPerTick);
    const auto tHop1 = std::chrono::steady_clock::now();
    dbgParMs += std::chrono::duration<double, std::milli>(tImp0 - tPar0).count();
    dbgImpactMs += std::chrono::duration<double, std::milli>(tHop0 - tImp0).count();
    dbgHopMs += std::chrono::duration<double, std::milli>(tHop1 - tHop0).count();
    dbgImpacts += nImp;

    for (SimWorker& w : simWorkers) {
        lastTickMoves += w.moves;
        dbgMoves += w.moves;
        for (uint32_t c : w.pending) {
            int x, y, z;
            unpackCell(c, x, y, z);
            tileNext[(size_t)tileOf(x, z)].v.push_back(c);
        }
        w.pending.clear();
        for (uint32_t c : w.dirtSeeds) dirtQueue.push_back(c);
        w.dirtSeeds.clear();
        for (uint32_t c : w.grassSeeds) {
            int x, y, z;
            unpackCell(c, x, y, z);
            const size_t gi = idx(x, y, z);
            if ((data[gi] & VOX_MASK) == M_GRASS && !(data[gi] & VOX_GACT)) {
                data[gi] |= VOX_GACT;
                grassActive.push_back(c);
            }
        }
        w.grassSeeds.clear();
    }
    analyzeDirt(kMaxDirtSeedsPerTick);
    if ((simTickCount % kGrassPeriod) == 0) growGrass(kGrassEntriesPerRun);
}

// Applies the water hops proposed during this tick. Returns the number applied.
int World::applyWaterHops(int maxHops) {
    int applied = 0;
    for (SimWorker& w : simWorkers) {
        for (uint64_t h : w.hops) {
            int sx, sy, sz, dx, dy, dz;
            unpackCell((uint32_t)(h >> 32), sx, sy, sz);
            unpackCell((uint32_t)(h & 0xFFFFFFFFu), dx, dy, dz);
            const size_t si = idx(sx, sy, sz);
            const uint8_t liquid = data[si] & VOX_MASK;
            if ((liquid != M_WATER && liquid != M_OIL) || applied >= maxHops || !cellFree(dx, dy, dz) || !cellFree(dx, dy - 1, dz)) {
                queueCell(sx, sy, sz);  // try again next tick
                continue;
            }
            while (cellFree(dx, dy - 1, dz)) dy--;  // land on the surface straight away
            const size_t di = idx(dx, dy, dz);
            data[di] = liquid;
            data[si] = 0;
            markDirtyWater(sx, sy, sz);
            markDirtyWater(dx, dy, dz);
            wakeAfterMove(sx, sy, sz);
            queueCell(dx, dy, dz);
            queueCell(dx, dy - 1, dz);
            applied++;
        }
        w.hops.clear();
    }
    return applied;
}

// Whatever rested against a cell that has just been vacated may now be able to move.
void World::wakeAfterMove(int sx, int sy, int sz) {
    queueCell(sx, sy + 1, sz);
    queueCell(sx + 1, sy + 1, sz);
    queueCell(sx - 1, sy + 1, sz);
    queueCell(sx, sy + 1, sz + 1);
    queueCell(sx, sy + 1, sz - 1);
    queueCell(sx + 1, sy, sz);
    queueCell(sx - 1, sy, sz);
    queueCell(sx, sy, sz + 1);
    queueCell(sx, sy, sz - 1);
}

// Serial pass over the water voxels that landed this tick (see the header comment). Returns the number handled.
int World::applyImpacts(int maxImpacts) {
    int applied = 0;
    for (SimWorker& w : simWorkers) {
        for (uint32_t rec : w.impacts) {
            int sx, sy, sz;
            unpackCell(rec & 0x7FFFFFFu, sx, sy, sz);
            const int power = (int)(rec >> 27);
            size_t si = idx(sx, sy, sz);
            if ((data[si] & VOX_MASK) != M_WATER) continue;
            if (applied >= maxImpacts) {
                queueCell(sx, sy, sz);
                continue;
            }
            if (cellFree(sx, sy - 1, sz)) {
                // What it landed on has soaked in a moment ago (a column of falling water arrives as a stack): it
                // drops into the gap right now, so the whole column goes in during the same tick.
                int ny = sy;
                while (sy - ny < kMaxFall && cellFree(sx, ny - 1, sz)) ny--;
                data[idx(sx, ny, sz)] = M_WATER | (uint8_t)(3 << VOX_ENERGY_SHIFT);
                data[si] = 0;
                markDirtyWater(sx, sy, sz);
                markDirtyWater(sx, ny, sz);
                wakeAfterMove(sx, sy, sz);
                if (cellFree(sx, ny - 1, sz)) {  // still in the air: back to the main loop
                    queueCell(sx, ny, sz);
                    continue;
                }
                sy = ny;
                si = idx(sx, sy, sz);
                data[si] &= (uint8_t)~VOX_ENERGY;
            }
            applied++;
            const bool pool = soakEnabled && get(sx, sy - 1, sz) == M_WATER;

            bool droplet = false;
            if (dropletBudget > 0) {
                const int pct = pool ? (power >= 3 ? 22 : 10) : (power >= 3 ? 12 : 5);
                if ((int)impactRng.below(100) < pct) {
                    droplet = true;
                    dropletBudget--;
                }
            }
            if (droplet || splashes.size() < kMaxSplashEvents)
                splashes.push_back({sx + 0.5f, (float)sy, sz + 0.5f, (uint8_t)power, (uint8_t)(droplet ? 1 : 0)});
            if (droplet) {  // the owner flies it and puts it back where it lands
                data[si] = 0;
                counts[M_WATER]--;
                counts[M_AIR]++;
                markDirtyWater(sx, sy, sz);
                wakeAfterMove(sx, sy, sz);
                continue;
            }
            if (!pool) {  // landed on the floor / sand / stone: the ordinary spreading rules take it from here
                queueCell(sx, sy, sz);
                continue;
            }

            // Soak in: take the lowest surface cell that is free among a few random spots around the impact (a short
            // way off first, then much further if the neighbourhood is crowded by other landing water).
            const int radius = 3 + 2 * power;
            int bx = 0, by = 0, bz = 0, bestDrop = -1000;
            for (int attempt = 0; attempt < 8; attempt++) {
                const int R = attempt < 6 ? radius : radius * 2;
                const int tx = sx + impactRng.below(2 * R + 1) - R, tz = sz + impactRng.below(2 * R + 1) - R;
                if (tx == sx && tz == sz) continue;
                int ty = sy, drop = 0;
                // Columns that have already taken a voxel this tick may be stacked a little (the pool levels out
                // right after); lower columns are better.
                while (ty < sy + 3 && get(tx, ty, tz) == M_WATER) {
                    ty++;
                    drop--;
                }
                if (!cellFree(tx, ty, tz)) continue;
                while (drop < 12 && cellFree(tx, ty - 1, tz)) {
                    ty--;
                    drop++;
                }
                if (get(tx, ty - 1, tz) != M_WATER) continue;  // only onto the pool, never onto dry ground
                if (drop > bestDrop) {
                    bestDrop = drop;
                    if (drop >= 1) {  // a lower surface: good enough
                        bx = tx;
                        by = ty;
                        bz = tz;
                        break;
                    }
                    bx = tx;
                    by = ty;
                    bz = tz;
                }
            }
            if (bestDrop == -1000) {  // no room anywhere around: the ordinary flow rules take it from here
                queueCell(sx, sy, sz);
                continue;
            }
            data[idx(bx, by, bz)] = M_WATER;
            data[si] = 0;
            markDirtyWater(sx, sy, sz);
            markDirtyWater(bx, by, bz);
            // Only the voxel stacked above can be affected by the vacated cell. The soaked voxel itself is not woken: it
            // went to the lowest spot nearby, and a top voxel's level scan is the costliest thing the automaton does.
            queueCell(sx, sy + 1, sz);
            queueCell(bx, by - 1, bz);
        }
        w.impacts.clear();
    }
    return applied;
}

void World::simTile(int tile, SimWorker& sw) {
    std::vector<uint32_t>& cur = tileCur[(size_t)tile].v;
    if (cur.empty()) return;
    Rng& rng = tileRng[(size_t)tile].r;
    int movesLocal = 0;

    // Counting sort by Y so the lowest voxels move first (columns fall as a block, not as a conveyor).
    int minY = 511, maxY = 0;
    for (uint32_t c : cur) {
        int y = (int)((c >> 9) & 511);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    const int span = maxY - minY + 1;
    sw.bucket.assign((size_t)span + 1, 0);
    for (uint32_t c : cur) sw.bucket[(size_t)((int)((c >> 9) & 511) - minY) + 1]++;
    for (int i = 0; i < span; i++) sw.bucket[(size_t)i + 1] += sw.bucket[(size_t)i];
    sw.sorted.resize(cur.size());
    {
        // `bucket[i]` is the start of row i; write through a moving cursor kept in the same array.
        sw.cursor.assign(sw.bucket.begin(), sw.bucket.begin() + span);
        for (uint32_t c : cur) sw.sorted[(size_t)sw.cursor[(size_t)((int)((c >> 9) & 511) - minY)]++] = c;
    }
    sw.moved.clear();

    auto qLocal = [&](int x, int y, int z) {
        if (!inBounds(x, y, z)) return;
        size_t i = idx(x, y, z);
        uint8_t v = data[i];
        if (isCellular(v & VOX_MASK) && !(v & VOX_QUEUED)) {
            data[i] = v | VOX_QUEUED;
            // Cells of the tile being processed are owned by this thread: queue them directly. Cells that
            // belong to a neighbouring tile are merged (serially) after the phase.
            if (tileOf(x, z) == tile) tileNext[(size_t)tile].v.push_back(packCell(x, y, z));
            else sw.pending.push_back(packCell(x, y, z));
        }
    };
    // Cells that may start moving once (x,y,z) becomes free: the one above, the four diagonal-above
    // neighbours (sand slides) and the four lateral neighbours (water spreads).
    auto wakeVacated = [&](int x, int y, int z) {
        qLocal(x, y + 1, z);
        qLocal(x + 1, y + 1, z);
        qLocal(x - 1, y + 1, z);
        qLocal(x, y + 1, z + 1);
        qLocal(x, y + 1, z - 1);
        qLocal(x + 1, y, z);
        qLocal(x - 1, y, z);
        qLocal(x, y, z + 1);
        qLocal(x, y, z - 1);
    };
    // `energy` < 0 keeps the mover's current energy, otherwise sets it (0..3).
    auto moveTo = [&](int x, int y, int z, int nx, int ny, int nz, int energy) {
        size_t si = idx(x, y, z), ti = idx(nx, ny, nz);
        const uint8_t sv = data[si];
        uint8_t sm = sv & (VOX_MASK | VOX_ENERGY), tm = data[ti] & VOX_MASK;
        if (energy >= 0) sm = (uint8_t)((sm & VOX_MASK) | (energy << VOX_ENERGY_SHIFT));
        data[ti] = (uint8_t)(sm | VOX_MOVED);
        data[si] = tm ? (uint8_t)(tm | VOX_MOVED) : (uint8_t)0;
        sw.moved.push_back((uint32_t)ti);
        if (tm) sw.moved.push_back((uint32_t)si);
        if (isLiquid(sm & VOX_MASK) && (tm == M_AIR || isLiquid(tm))) {
            markDirtyWater(x, y, z);
            markDirtyWater(nx, ny, nz);
        } else {
            markDirty(x, y, z);
            markDirty(nx, ny, nz);
        }
        wakeVacated(x, y, z);
        if (tm) qLocal(x, y, z);
        qLocal(nx, ny, nz);
        qLocal(nx, ny - 1, nz);  // whatever rests below the new position now carries extra weight
        movesLocal++;
    };
    // Lowest free cell reachable by falling at most kMaxFall cells from (x,y,z); y itself if blocked.
    auto fallTarget = [&](int x, int y, int z) {
        int ny = y;
        while (y - ny < kMaxFall && cellFree(x, ny - 1, z)) ny--;
        return ny;
    };

    const bool backwards = (simTickCount & 1) != 0;
    for (int row = 0; row < span; row++) {
        const int begin = sw.bucket[(size_t)row], end = sw.bucket[(size_t)row + 1];
        for (int k = 0; k < end - begin; k++) {
            const uint32_t c = sw.sorted[(size_t)(backwards ? (end - 1 - k) : (begin + k))];
            int x, y, z;
            unpackCell(c, x, y, z);
            const size_t i = idx(x, y, z);
            const uint8_t v = data[i];
            data[i] = v & (uint8_t)~VOX_QUEUED;
            if (v & VOX_MOVED) continue;
            const uint8_t m = v & VOX_MASK;

            if (m == M_SAND) {
                int ny = fallTarget(x, y, z);
                if (ny != y) {
                    moveTo(x, y, z, x, ny, z, -1);
                    continue;
                }
                if (inBounds(x, y - 1, z)) {  // sand sinks through water (swap)
                    size_t bi = idx(x, y - 1, z);
                    if (((data[bi] & VOX_MASK) == M_WATER || (data[bi] & VOX_MASK) == M_OIL) && !(data[bi] & VOX_MOVED) && (!overlayActive || overlay[bi] == 0)) {
                        moveTo(x, y, z, x, y - 1, z, -1);
                        continue;
                    }
                }
                // Clumpy sand: a grain only slides off when the drop beside it is at least `need` cells deep.
                // `need` is a fixed property of the grain's position (a hash), so some grains are loose and
                // others stick, which gives steep, rough piles instead of a smooth 45 degree cone; a grain
                // that is held by three or more neighbours (a clump) sticks one cell harder.
                int cand[4], n = 0;
                int need = 0;
                bool heldBack = false;  // could slide but the drop beside it is not deep enough yet
                for (int d = 0; d < 4; d++) {
                    int nx = x + kDX[d], nz = z + kDZ[d];
                    if (!cellFree(nx, y, nz) || !cellFree(nx, y - 1, nz)) continue;
                    if (need == 0) {
                        uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
                        h ^= h >> 13;
                        h *= 0x5bd1e995u;
                        h ^= h >> 15;
                        const uint32_t r = h & 15u;
                        need = r < kLooseGrains ? 1 : (r < kLooseGrains + kMediumGrains ? 2 : 3);
                        int held = 0;
                        for (int e = 0; e < 4; e++) {
                            const uint8_t nm = get(x + kDX[e], y, z + kDZ[e]);
                            held += isOpaque(nm);
                        }
                        if (held >= 3) need++;
                    }
                    bool ok = true;
                    for (int k2 = 2; k2 <= need && ok; k2++) ok = cellFree(nx, y - k2, nz);
                    if (ok) cand[n++] = d;
                    else heldBack = true;
                }
                if (n == 0 && heldBack && rng.below(16) == 0) qLocal(x, y, z);  // a deeper drop may open up later
                if (n) {
                    if (rng.below(4) == 0) {
                        qLocal(x, y, z);  // granular flow: an unstable grain lets go with ~75% chance per tick
                    } else {
                        int d = cand[rng.below(n)];
                        moveTo(x, y, z, x + kDX[d], y - 1, z + kDZ[d], -1);
                    }
                }
            } else if (m == M_DIRT || m == M_GRASS) {
                if (m == M_GRASS || !(v & VOX_LOOSE)) {
                    // Settled dirt / grass is static. It is only looked at when something around it changed: grass may be
                    // able to spread again, and a voxel with nothing under it may be hanging (-> structure analysis).
                    if (m == M_GRASS && !(v & VOX_GACT)) sw.grassSeeds.push_back(c);
                    const uint8_t bm = get(x, y - 1, z);
                    if (bm == M_AIR || isLiquid(bm)) sw.dirtSeeds.push_back(c);
                    continue;
                }
                // Loose dirt falls like sand, sinks through water, and slides only off steep drops: it is much clumpier
                // than sand (a grain needs a 3-5 cell drop beside it). Where it cannot move it settles for good.
                int ny = fallTarget(x, y, z);
                if (ny != y) {
                    moveTo(x, y, z, x, ny, z, -1);
                    continue;
                }
                if (inBounds(x, y - 1, z)) {
                    size_t bi = idx(x, y - 1, z);
                    if (((data[bi] & VOX_MASK) == M_WATER || (data[bi] & VOX_MASK) == M_OIL) && !(data[bi] & VOX_MOVED) && (!overlayActive || overlay[bi] == 0)) {
                        moveTo(x, y, z, x, y - 1, z, -1);
                        continue;
                    }
                }
                int cand[4], n = 0, need = 0;
                for (int d = 0; d < 4; d++) {
                    int nx = x + kDX[d], nz = z + kDZ[d];
                    if (!cellFree(nx, y, nz) || !cellFree(nx, y - 1, nz)) continue;
                    if (need == 0) {
                        uint32_t h = (uint32_t)x * 2654435761u ^ (uint32_t)y * 40503u ^ (uint32_t)z * 2246822519u;
                        h ^= h >> 15;
                        h *= 0x2c1b3c6du;
                        h ^= h >> 12;
                        need = 3 + (int)((h & 7u) < 3 ? 0 : ((h & 7u) < 6 ? 1 : 2));  // 3, 4 or 5
                        int held = 0;
                        for (int e = 0; e < 4; e++) {
                            const uint8_t nm = get(x + kDX[e], y, z + kDZ[e]);
                            held += isOpaque(nm);
                        }
                        if (held >= 3) need++;
                    }
                    bool ok = true;
                    for (int k2 = 2; k2 <= need && ok; k2++) ok = cellFree(nx, y - k2, nz);
                    if (ok) cand[n++] = d;
                }
                if (n) {
                    if (rng.below(4) == 0) {
                        qLocal(x, y, z);
                    } else {
                        int d = cand[rng.below(n)];
                        moveTo(x, y, z, x + kDX[d], y - 1, z + kDZ[d], -1);
                    }
                } else {
                    data[i] &= (uint8_t)~VOX_LOOSE;  // at rest: static from now on
                    const uint8_t bm = get(x, y - 1, z);
                    if (bm != M_SAND && !isRigidMat(bm) && bm != M_BEDROCK && y > 0) sw.dirtSeeds.push_back(c);  // (it may now load an overhang)
                }
            } else if (m == M_WATER || m == M_OIL) {
                const bool oil = m == M_OIL;
                const int fall = (v & VOX_ENERGY) >> VOX_ENERGY_SHIFT;  // consecutive ticks spent free-falling (max 3)
                int ny = fallTarget(x, y, z);
                if (ny != y) {
                    moveTo(x, y, z, x, ny, z, std::min(3, fall + 1));
                    continue;
                }
                if (fall) {  // it has just landed
                    data[i] &= (uint8_t)~VOX_ENERGY;
                    // (water pouring into the hollow around the player is not a splash: the player's own splash handles that)
                    if (!oil && fall >= 2 && impactsEnabled &&
                        !(boxOn && x >= boxMin[0] - 8 && x <= boxMax[0] + 8 && z >= boxMin[2] - 8 && z <= boxMax[2] + 8)) {
                        sw.impacts.push_back(packCell(x, y, z) | ((uint32_t)fall << 27));
                        if (soakEnabled && get(x, y - 1, z) == M_WATER) continue;  // on a pool: the serial pass soaks it in
                    }
                }
                if (!oil && inBounds(x, y - 1, z)) {  // water sinks through oil (oil floats on it)
                    const size_t bi = idx(x, y - 1, z);
                    if ((data[bi] & VOX_MASK) == M_OIL && !(data[bi] & VOX_MOVED) && (!overlayActive || overlay[bi] == 0)) {
                        moveTo(x, y, z, x, y - 1, z, 0);
                        continue;
                    }
                }
                const bool top = cellFree(x, y + 1, z);
                // 1) Level the surface: look along the four axes, straight through other water, for a column
                //    at least two lower than this one, and hop to it.
                if (top) {
                    int bestK = oil ? kOilReach : kWaterReach, best[8], nb = 0;
                    for (int d = 0; d < 8; d++) {
                        for (int kk = 1; kk <= bestK; kk++) {
                            const int nx = x + kLineDX[d] * kk, nz = z + kLineDZ[d] * kk;
                            if (!inBounds(nx, y, nz)) break;
                            const size_t ci = idx(nx, y, nz);
                            const uint8_t mm = data[ci] & VOX_MASK;
                            if (mm == m) continue;            // flows through more of itself
                            if (mm != M_AIR) break;            // wall / sand
                            if ((overlayActive && overlay[ci]) || (boxOn && inPlayerBox(nx, y, nz))) break;
                            if (cellFree(nx, y - 1, nz)) {     // the column there is lower by >= 2
                                if (kk < bestK) nb = 0;
                                bestK = kk;
                                best[nb++] = d;
                                break;
                            }
                        }
                    }
                    if (nb) {
                        if ((int)rng.below(100) < (oil ? kOilHopPercent : kHopPercent)) {
                            const int d = best[rng.below(nb)];
                            const uint32_t src = packCell(x, y, z), dst = packCell(x + kLineDX[d] * bestK, y, z + kLineDZ[d] * bestK);
                            sw.hops.push_back(((uint64_t)src << 32) | dst);
                        } else {
                            qLocal(x, y, z);  // not this tick
                        }
                        continue;
                    }
                }
                // 2) Pressure: a voxel with water above it that has open space beside it (the edge of a mound)
                //    squeezes sideways.
                int cand[4], n = 0;
                for (int dd = 0; dd < 4; dd++)
                    if (cellFree(x + kDX[dd], y, z + kDZ[dd])) cand[n++] = dd;
                if (n && isCellular(get(x, y + 1, z))) {
                    if (oil && (int)rng.below(100) >= kOilMovePercent) {  // thick: it creeps
                        qLocal(x, y, z);
                        continue;
                    }
                    const int dd = cand[rng.below(n)];
                    moveTo(x, y, z, x + kDX[dd], y, z + kDZ[dd], 0);
                } else if (n && top && rng.below(24) == 0) {
                    // A lower surface may appear further away than the wake radius reaches (a hole opens in
                    // the floor), so water on the rim of a pool re-checks itself now and then.
                    qLocal(x, y, z);
                }
            }
        }
    }

    for (uint32_t i : sw.moved) data[i] &= (uint8_t)~VOX_MOVED;
    sw.moves += movesLocal;
    cur.clear();
}
