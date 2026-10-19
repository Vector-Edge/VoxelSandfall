// Cellular automata for the two particle materials (spec section 1):
//   Sand  - falls straight down; if blocked, slides diagonally (down-left/right/forward/back).
//   Water - falls straight down; if blocked, spreads laterally into adjacent empty cells.
// Only voxels on the active lists are visited, so a settled world costs nothing.
//
// Parallelism: the grid is cut into 8x8 voxel columns ("tiles", full height). Tiles of the same
// checkerboard colour are at least one idle tile (8 voxels) apart, and a voxel only touches cells within
// kWaterReach+2 voxels, so the four colours are simulated one after another while all tiles of a colour
// run concurrently on the thread pool. Newly woken cells are collected per worker and merged afterwards.
#include "Parallel.h"
#include "World.h"

namespace {
constexpr int kDX[4] = {1, -1, 0, 0};
constexpr int kDZ[4] = {0, 0, 1, -1};
constexpr int kWaterReach = 5;  // how far a water voxel looks for a lower surface to flow toward
constexpr int kMaxFall = 3;     // cells a free-falling voxel drops per tick
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
    if (total == 0) return;

    ThreadPool& pool = globalPool();
    if (simWorkers.size() != (size_t)pool.participants()) simWorkers.resize((size_t)pool.participants());
    for (SimWorker& w : simWorkers) w.moves = 0;

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

    for (SimWorker& w : simWorkers) {
        lastTickMoves += w.moves;
        for (uint32_t c : w.pending) {
            int x, y, z;
            unpackCell(c, x, y, z);
            tileNext[(size_t)tileOf(x, z)].v.push_back(c);
        }
        w.pending.clear();
    }
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
        markDirty(x, y, z);
        markDirty(nx, ny, nz);
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
                    if ((data[bi] & VOX_MASK) == M_WATER && !(data[bi] & VOX_MOVED) && (!overlayActive || overlay[bi] == 0)) {
                        moveTo(x, y, z, x, y - 1, z, -1);
                        continue;
                    }
                }
                int cand[4], n = 0;
                for (int d = 0; d < 4; d++) {
                    int nx = x + kDX[d], nz = z + kDZ[d];
                    if (cellFree(nx, y, nz) && cellFree(nx, y - 1, nz)) cand[n++] = d;
                }
                if (n) {
                    int d = cand[rng.below(n)];
                    moveTo(x, y, z, x + kDX[d], y - 1, z + kDZ[d], -1);
                }
            } else if (m == M_WATER) {
                int ny = fallTarget(x, y, z);
                if (ny != y) {
                    moveTo(x, y, z, x, ny, z, 3);
                    continue;
                }
                // 1) flow toward the nearest drop (ledge / lower surface) within reach
                int bestK = 99, best[4], nb = 0;
                for (int d = 0; d < 4; d++) {
                    for (int kk = 1; kk <= kWaterReach; kk++) {
                        int nx = x + kDX[d] * kk, nz = z + kDZ[d] * kk;
                        if (!cellFree(nx, y, nz)) break;
                        if (cellFree(nx, y - 1, nz)) {
                            if (kk < bestK) { bestK = kk; nb = 0; }
                            if (kk == bestK) best[nb++] = d;
                            break;
                        }
                    }
                }
                int d = -1, step = 1;
                if (nb) {
                    d = best[rng.below(nb)];
                    if (bestK >= 2) step = 1 + rng.below(2);  // the cells up to the drop are free
                } else if (isCellular(get(x, y + 1, z))) {
                    // 2) pressure from above: spread sideways into any empty neighbour to flatten the column
                    int cand[4], n = 0;
                    for (int dd = 0; dd < 4; dd++)
                        if (cellFree(x + kDX[dd], y, z + kDZ[dd])) cand[n++] = dd;
                    if (n) d = cand[rng.below(n)];
                }
                if (d >= 0) {
                    moveTo(x, y, z, x + kDX[d] * step, y, z + kDZ[d] * step, 3);  // directed flow re-energises the voxel
                } else {
                    // Water on the free surface re-checks itself now and then because a lower surface may
                    // appear further away than the wake radius reaches. Voxels resting on other water diffuse
                    // sideways for a limited time (their 2-bit energy runs down), which flattens 1-voxel
                    // terraces without keeping the pool - and its meshes - busy forever.
                    int cand[4], n = 0;
                    for (int dd = 0; dd < 4; dd++)
                        if (cellFree(x + kDX[dd], y, z + kDZ[dd])) cand[n++] = dd;
                    if (n) {
                        const int energy = (v & VOX_ENERGY) >> VOX_ENERGY_SHIFT;
                        if (energy > 0 && get(x, y - 1, z) == M_WATER) {
                            int dd = cand[rng.below(n)];
                            moveTo(x, y, z, x + kDX[dd], y, z + kDZ[dd], rng.below(5) == 0 ? energy - 1 : energy);
                        } else if (rng.below(24) == 0) {
                            qLocal(x, y, z);
                        }
                    }
                }
            }
        }
    }

    for (uint32_t i : sw.moved) data[i] &= (uint8_t)~VOX_MOVED;
    sw.moves += movesLocal;
    cur.clear();
}
