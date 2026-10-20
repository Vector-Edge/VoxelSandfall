// Chunk loading: an (almost) infinite world seen through a sliding window of 15 x 15 regions.
//
// The voxel grid (World::data) always holds the region the player is standing in plus seven regions in every direction. A
// region is one chunk column (32 x 32 columns) and the full 256 height. Coordinates inside the grid stay "local"
// (0..479) forever, so the simulation, physics, player, shaders etc. never see big numbers; World::originX / originZ say
// which global voxel local (0, 0) is. The distance fog hides the window's edge completely, and when the player walks into a
// neighbouring region the window slides by one region (a row of 15 regions out, a row in):
//
//   * the grid is NOT copied: a small table maps every logical chunk column of the window to a physical block of
//     storage (colSlot_), so recentring only permutes the table;
//   * regions that fall off the window are saved if they were ever modified (run-length compressed, in memory) and their
//     storage is reused for the regions that come into view, which are restored from the store or generated (a flat
//     Permanent Stone floor, nothing else, for now);
//   * everything that holds local coordinates (the sim's active lists, dirt / grass lists) is rebased; the owner
//     (Game) shifts the player, rigid bodies, particles and the renderer shifts its meshes and textures.
#include <algorithm>
#include <cstring>

#include "World.h"

namespace {

inline uint64_t regionKey(long long rx, long long rz) { return ((uint64_t)(uint32_t)(int32_t)rx << 32) | (uint64_t)(uint32_t)(int32_t)rz; }

// What is kept of a voxel byte when it goes to the store: the material, plus "loose" for falling dirt (the queued /
// moved / water-energy / grass-active bits are transient and are rebuilt on load).
inline uint8_t normByte(uint8_t b) {
    const uint8_t m = b & VOX_MASK;
    return m == M_DIRT ? (uint8_t)(m | (b & VOX_LOOSE)) : m;
}

bool allZero(const uint8_t* p, size_t n) {
    const uint64_t* w = (const uint64_t*)p;
    for (size_t i = 0; i < n / 8; i++)
        if (w[i]) return false;
    return true;
}

constexpr uint64_t kAllBedrock = 0x0404040404040404ull;  // eight bedrock voxels

// Nothing in the chunk can move: it holds only air, stone and bedrock (the common case by far: floor chunks and the sky).
bool chunkIsInert(const uint8_t* p) {
    const uint64_t* w = (const uint64_t*)p;
    for (size_t i = 0; i < 4096; i++) {
        const uint64_t v = w[i];
        if (v == 0 || v == kAllBedrock) continue;
        for (int k = 0; k < 8; k++) {
            const uint8_t m = (uint8_t)(v >> (8 * k)) & VOX_MASK;
            if (isCellular(m) || m == M_BCOAL || m == M_BOIL) return false;
        }
    }
    return true;
}

}  // namespace

void World::resetColumns() {
    for (int i = 0; i < NCOL; i++) colSlot_[i] = colLogical_[i] = i;
}

// ---- storage helpers --------------------------------------------------------------------------------------------
uint8_t* World::physChunk(int physCol, int cy) { return data.data() + (((size_t)physCol * CY + (size_t)cy) << 15); }

void World::regionCounts(int rxl, int rzl, long long sign) {
    long long hist[16] = {0};
    for (int dz = 0; dz < REGION_CHUNKS; dz++)
        for (int dx = 0; dx < REGION_CHUNKS; dx++) {
            const int col = colSlot_[(rxl * REGION_CHUNKS + dx) + CX * (rzl * REGION_CHUNKS + dz)];
            for (int cy = 0; cy < CY; cy++) {
                const uint8_t* c = physChunk(col, cy);
                const uint64_t* w = (const uint64_t*)c;
                for (int i = 0; i < 4096; i++) {
                    if (w[i] == 0) {
                        hist[0] += 8;
                    } else {
                        for (int k = 0; k < 8; k++) hist[(uint8_t)(w[i] >> (8 * k)) & VOX_MASK]++;
                    }
                }
            }
        }
    for (int m = 0; m < 16; m++) counts[m] += sign * hist[m];
}

// What a freshly generated region adds to the voxel counts (its storage holds nothing but the floor).
void World::generatedCounts(long long sign) {
    const long long cols = (long long)REGION_CHUNKS * REGION_CHUNKS, floor = cols * 1024;
    counts[M_BEDROCK] += sign * floor;
    counts[M_AIR] += sign * (cols * (long long)CY * 32768 - floor);
}

void World::generateRegionInto(int rxl, int rzl) {
    for (int dz = 0; dz < REGION_CHUNKS; dz++)
        for (int dx = 0; dx < REGION_CHUNKS; dx++) {
            const int col = colSlot_[(rxl * REGION_CHUNKS + dx) + CX * (rzl * REGION_CHUNKS + dz)];
            std::memset(physChunk(col, 0), 0, (size_t)CY << 15);
            uint8_t* floorChunk = physChunk(col, 0);  // the flat Permanent Stone floor: one voxel thick at y = 0
            for (int z = 0; z < 32; z++) std::memset(floorChunk + ((size_t)z << 10), M_BEDROCK, 32);
        }
}

void World::saveRegionTo(int rxl, int rzl, std::vector<uint8_t>& out) {
    out.clear();
    auto put32 = [&](uint32_t v) {
        for (int k = 0; k < 4; k++) out.push_back((uint8_t)(v >> (8 * k)));
    };
    for (int dz = 0; dz < REGION_CHUNKS; dz++)
        for (int dx = 0; dx < REGION_CHUNKS; dx++) {
            const int col = colSlot_[(rxl * REGION_CHUNKS + dx) + CX * (rzl * REGION_CHUNKS + dz)];
            for (int cy = 0; cy < CY; cy++) {
                const uint8_t* c = physChunk(col, cy);
                const size_t lenAt = out.size();
                put32(0);
                if (allZero(c, 32768)) {
                    out.push_back(0);
                    out.push_back(0x00);
                    out.push_back(0x80);  // value 0, run 32768
                } else {
                    size_t i = 0;
                    while (i < 32768) {
                        const uint8_t v = normByte(c[i]);
                        size_t j = i + 1;
                        if (v == 0)
                            while (j + 8 <= 32768 && j - i < 65520 && *(const uint64_t*)(c + j) == 0) j += 8;
                        while (j < 32768 && j - i < 65535 && normByte(c[j]) == v) j++;
                        out.push_back(v);
                        out.push_back((uint8_t)((j - i) & 255));
                        out.push_back((uint8_t)((j - i) >> 8));
                        i = j;
                    }
                }
                const uint32_t len = (uint32_t)(out.size() - lenAt - 4);
                for (int k = 0; k < 4; k++) out[lenAt + (size_t)k] = (uint8_t)(len >> (8 * k));
            }
        }
}

void World::loadRegionFrom(int rxl, int rzl, const std::vector<uint8_t>& in) {
    size_t p = 0;
    for (int dz = 0; dz < REGION_CHUNKS; dz++)
        for (int dx = 0; dx < REGION_CHUNKS; dx++) {
            const int col = colSlot_[(rxl * REGION_CHUNKS + dx) + CX * (rzl * REGION_CHUNKS + dz)];
            for (int cy = 0; cy < CY; cy++) {
                uint32_t len = 0;
                for (int k = 0; k < 4; k++) len |= (uint32_t)in[p + (size_t)k] << (8 * k);
                p += 4;
                uint8_t* c = physChunk(col, cy);
                size_t o = 0, q = p;
                while (q < p + len && o < 32768) {
                    const uint8_t v = in[q];
                    const size_t run = (size_t)in[q + 1] | ((size_t)in[q + 2] << 8);
                    std::memset(c + o, v, std::min(run, (size_t)32768 - o));
                    o += run;
                    q += 3;
                }
                p += len;
            }
        }
}

// After a region has been put into the window: everything in it that could still be moving is woken.
void World::wakeRegion(int rxl, int rzl) {
    for (int dz = 0; dz < REGION_CHUNKS; dz++)
        for (int dx = 0; dx < REGION_CHUNKS; dx++) {
            const int cxl = rxl * REGION_CHUNKS + dx, czl = rzl * REGION_CHUNKS + dz;
            const int col = colSlot_[cxl + CX * czl];
            for (int cy = 0; cy < CY; cy++) {
                const uint8_t* c = physChunk(col, cy);
                if (chunkIsInert(c)) continue;
                for (int z = 0; z < 32; z++)
                    for (int y = 0; y < 32; y++)
                        for (int x = 0; x < 32; x++) {
                            const uint8_t b = c[(z << 10) + (y << 5) + x];
                            const uint8_t m = b & VOX_MASK;
                            if (m == M_AIR || m == M_STONE || m == M_BEDROCK || m == M_COAL) continue;
                            const int gx = cxl * 32 + x, gy = cy * 32 + y, gz = czl * 32 + z;
                            if (m == M_BCOAL || m == M_BOIL) {  // it was saved while burning: it carries on
                                registerBurning(gx, gy, gz, -1.0f);
                                continue;
                            }
                            if (m == M_GRASS) {
                                data[idx(gx, gy, gz)] |= VOX_GACT;
                                grassActive.push_back(packCell(gx, gy, gz));
                                continue;
                            }
                            if (m == M_SAND || m == M_WATER || m == M_DIRT || m == M_OIL) {
                                if (get(gx + 1, gy, gz) == M_AIR || get(gx - 1, gy, gz) == M_AIR || get(gx, gy + 1, gz) == M_AIR ||
                                    get(gx, gy - 1, gz) == M_AIR || get(gx, gy, gz + 1) == M_AIR || get(gx, gy, gz - 1) == M_AIR)
                                    queueCell(gx, gy, gz);
                            }
                        }
            }
        }
}

// Water / sand that had been piling up against the edge of the old window can flow into the new region now.
void World::wakeSeam(int rxl, int rzl, int side) {  // side: 0 = -x, 1 = +x, 2 = -z, 3 = +z of the region (rxl, rzl)
    const int x0 = rxl * REGION, z0 = rzl * REGION;
    for (int s = 0; s < REGION_CHUNKS; s++)      // the chunks along the seam (in the neighbouring, retained region)
        for (int cy = 0; cy < CY; cy++) {
            int cxl, czl;
            switch (side) {
                case 0: cxl = (x0 - 1) >> 5; czl = (z0 >> 5) + s; break;
                case 1: cxl = (x0 + REGION) >> 5; czl = (z0 >> 5) + s; break;
                case 2: cxl = (x0 >> 5) + s; czl = (z0 - 1) >> 5; break;
                default: cxl = (x0 >> 5) + s; czl = (z0 + REGION) >> 5; break;
            }
            if (!chunkModified[(size_t)chunkIndex(cxl, cy, czl)]) continue;  // untouched since generated: only a floor
            if (chunkIsInert(physChunk(colSlot_[cxl + CX * czl], cy))) continue;
            for (int a = 0; a < 32; a++)
                for (int yy = 0; yy < 32; yy++) {
                    int x, z;
                    switch (side) {
                        case 0: x = x0 - 1; z = (czl << 5) + a; break;
                        case 1: x = x0 + REGION; z = (czl << 5) + a; break;
                        case 2: x = (cxl << 5) + a; z = z0 - 1; break;
                        default: x = (cxl << 5) + a; z = z0 + REGION; break;
                    }
                    const int y = cy * 32 + yy;
                    const uint8_t m = get(x, y, z);
                    if (m == M_SAND || m == M_WATER || m == M_DIRT || m == M_OIL) queueCell(x, y, z);
                }
        }
}

// ---- window management ------------------------------------------------------------------------------------------
void World::initStreaming() {
    epoch++;
    streaming = true;
    originX = -(long long)REGION * CENTER;  // global (0, 0) is the corner of the middle region
    originZ = -(long long)REGION * CENTER;
    savedRegions.clear();
    resetColumns();
    std::fill(overlay.begin(), overlay.end(), (uint8_t)0);
    overlayActive = false;
    for (auto& t : tileCur) t.v.clear();
    for (auto& t : tileNext) t.v.clear();
    dirtQueue.clear();
    dirtHead = 0;
    grassActive.clear();
    burning.clear();
    burnCursor = 0;
    oilFront.clear();
    grassCursor = 0;
    splashes.clear();
    for (auto& c : counts) c = 0;
    std::fill(data.begin(), data.end(), (uint8_t)0);
    for (int rz = 0; rz < WIN_REGIONS; rz++)
        for (int rx = 0; rx < WIN_REGIONS; rx++) generateRegionInto(rx, rz);
    for (int rz = 0; rz < WIN_REGIONS; rz++)
        for (int rx = 0; rx < WIN_REGIONS; rx++) regionCounts(rx, rz, +1);
    markAllDirty();
    std::fill(chunkModified.begin(), chunkModified.end(), (uint8_t)0);
    std::fill(chunkUrgent.begin(), chunkUrgent.end(), (uint8_t)1);
}

bool World::regionLoaded(long long rx, long long rz) const {
    const long long lx = rx - originX / REGION, lz = rz - originZ / REGION;
    return lx >= 0 && lx < WIN_REGIONS && lz >= 0 && lz < WIN_REGIONS;
}

World::ShiftResult World::shiftWindow(int dRx, int dRz) {
    ShiftResult res;
    if (!streaming || (dRx == 0 && dRz == 0)) return res;
    const int NR = WIN_REGIONS, RC = REGION_CHUNKS;
    const long long baseRx = originX / REGION, baseRz = originZ / REGION;

    // 1) regions that leave the window: saved if they were ever modified, then their storage is free
    std::vector<int> freeCols;
    for (int rzl = 0; rzl < NR; rzl++)
        for (int rxl = 0; rxl < NR; rxl++) {
            const int nrx = rxl - dRx, nrz = rzl - dRz;
            if (nrx >= 0 && nrx < NR && nrz >= 0 && nrz < NR) continue;
            bool modified = false;
            for (int dz = 0; dz < RC; dz++)
                for (int dx = 0; dx < RC; dx++)
                    for (int cy = 0; cy < CY; cy++)
                        modified |= chunkModified[(size_t)chunkIndex(rxl * RC + dx, cy, rzl * RC + dz)] != 0;
            const uint64_t key = regionKey(baseRx + rxl, baseRz + rzl);
            if (modified) {
                std::vector<uint8_t>& blob = savedRegions[key];
                saveRegionTo(rxl, rzl, blob);
            }
            // (an untouched region is exactly what the generator makes: no need to look at its voxels to uncount them)
            if (modified) regionCounts(rxl, rzl, -1);
            else generatedCounts(-1);
            for (int dz = 0; dz < RC; dz++)
                for (int dx = 0; dx < RC; dx++) freeCols.push_back(colSlot_[(rxl * RC + dx) + CX * (rzl * RC + dz)]);
            res.evicted.push_back({baseRx + rxl, baseRz + rzl});
        }

    // 2) the new column table (retained columns keep their storage) and the per-chunk flags, permuted the same way
    int newSlot[NCOL];
    size_t freeNext = 0;
    auto oldColOf = [&](int cxl, int czl, int& ox, int& oz) {
        ox = cxl + RC * dRx;
        oz = czl + RC * dRz;
        return ox >= 0 && ox < CX && oz >= 0 && oz < CZ;
    };
    for (int czl = 0; czl < CZ; czl++)
        for (int cxl = 0; cxl < CX; cxl++) {
            int ox, oz;
            newSlot[cxl + CX * czl] = oldColOf(cxl, czl, ox, oz) ? colSlot_[ox + CX * oz] : freeCols[freeNext++];
        }
    auto permute = [&](std::vector<uint8_t>& v, size_t stride, uint8_t fill) {
        std::vector<uint8_t> nv(v.size(), fill);
        for (int cz = 0; cz < CZ; cz++)
            for (int cy = 0; cy < CY; cy++)
                for (int cx = 0; cx < CX; cx++) {
                    int ox, oz;
                    if (!oldColOf(cx, cz, ox, oz)) continue;
                    std::memcpy(&nv[(size_t)chunkIndex(cx, cy, cz) * stride], &v[(size_t)chunkIndex(ox, cy, oz) * stride], stride);
                }
        v.swap(nv);
    };
    permute(meshDirty, 1, 1);
    permute(texDirty, 1, 1);
    permute(waterDirty, 1, 0);
    permute(summaryDirty, 1, 1);
    permute(chunkModified, 1, 0);
    permute(chunkUrgent, 1, 1);
    permute(brickSolid, 64, 0);
    for (int i = 0; i < NCOL; i++) colSlot_[i] = newSlot[i];
    for (int i = 0; i < NCOL; i++) colLogical_[colSlot_[i]] = i;
    originX += (long long)dRx * REGION;
    originZ += (long long)dRz * REGION;

    // 3) regions that come into the window: restored from the store, or generated
    for (int rzl = 0; rzl < NR; rzl++)
        for (int rxl = 0; rxl < NR; rxl++) {
            const int orx = rxl + dRx, orz = rzl + dRz;
            if (orx >= 0 && orx < NR && orz >= 0 && orz < NR) continue;
            const uint64_t key = regionKey(originX / REGION + rxl, originZ / REGION + rzl);
            auto it = savedRegions.find(key);
            if (it != savedRegions.end()) {
                loadRegionFrom(rxl, rzl, it->second);
                regionCounts(rxl, rzl, +1);
                for (int dz = 0; dz < RC; dz++)  // restored chunks differ from the generator's: they count as modified (saved again, counted by scanning)
                    for (int dx = 0; dx < RC; dx++)
                        for (int cy = 0; cy < CY; cy++) chunkModified[(size_t)chunkIndex(rxl * RC + dx, cy, rzl * RC + dz)] = 1;
                res.restored.push_back({originX / REGION + rxl, originZ / REGION + rzl});
            } else {
                generateRegionInto(rxl, rzl);
                generatedCounts(+1);
            }
            res.loaded.push_back({originX / REGION + rxl, originZ / REGION + rzl});
        }

    // 4) everything that stores local coordinates moves with the origin
    const int dx = dRx * REGION, dz = dRz * REGION;
    {
        std::vector<TileList> old(std::move(tileNext));
        tileNext.assign((size_t)TX * TZ, TileList());
        for (auto& t : tileCur) t.v.clear();
        for (const TileList& t : old)
            for (uint32_t c : t.v) {
                int x, y, z;
                unpackCell(c, x, y, z);
                x -= dx;
                z -= dz;
                if ((unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) tileNext[(size_t)tileOf(x, z)].v.push_back(packCell(x, y, z));
            }
    }
    {
        std::vector<uint32_t> keep;
        for (size_t i = dirtHead; i < dirtQueue.size(); i++) {
            int x, y, z;
            unpackCell(dirtQueue[i], x, y, z);
            x -= dx;
            z -= dz;
            if ((unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) keep.push_back(packCell(x, y, z));
        }
        dirtQueue.swap(keep);
        dirtHead = 0;
        keep.clear();
        for (uint32_t c : grassActive) {
            int x, y, z;
            unpackCell(c, x, y, z);
            x -= dx;
            z -= dz;
            if ((unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) keep.push_back(packCell(x, y, z));
        }
        grassActive.swap(keep);
        // burning coal / oil moves with the window (what left it was saved as burning)
        {
            std::vector<BurnCell> kept;
            kept.reserve(keep.size());
            for (const BurnCell& b : burning) {
                int x, y, z;
                unpackCell(b.cell, x, y, z);
                x -= dx;
                z -= dz;
                if ((unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) kept.push_back({packCell(x, y, z), b.left, b.age});
            }
            burning.swap(kept);
            burnCursor = 0;
            std::vector<uint32_t> front;  // (the wave front too)
            for (uint32_t c : oilFront) {
                int x, y, z;
                unpackCell(c, x, y, z);
                x -= dx;
                z -= dz;
                if ((unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) front.push_back(packCell(x, y, z));
            }
            oilFront.swap(front);
        }
        grassCursor = 0;
    }
    for (SimWorker& w : simWorkers) {
        w.pending.clear();
        w.hops.clear();
        w.impacts.clear();
        w.dirtSeeds.clear();
        w.grassSeeds.clear();
    }
    splashes.clear();
    if (boxOn) {
        boxMin[0] -= dx; boxMax[0] -= dx;
        boxMin[2] -= dz; boxMax[2] -= dz;
    }

    // 5) wake what could be moving in the new regions, and what was held back by the old edge of the window
    for (const auto& r : res.loaded) {
        const int rxl = (int)(r.first - originX / REGION), rzl = (int)(r.second - originZ / REGION);
        if (std::find(res.restored.begin(), res.restored.end(), r) != res.restored.end()) wakeRegion(rxl, rzl);  // (generated ones hold nothing that moves)
        if (rxl > 0 && !regionIsNew(res, rxl - 1, rzl)) wakeSeam(rxl, rzl, 0);
        if (rxl < NR - 1 && !regionIsNew(res, rxl + 1, rzl)) wakeSeam(rxl, rzl, 1);
        if (rzl > 0 && !regionIsNew(res, rxl, rzl - 1)) wakeSeam(rxl, rzl, 2);
        if (rzl < NR - 1 && !regionIsNew(res, rxl, rzl + 1)) wakeSeam(rxl, rzl, 3);
    }
    overlayActive = false;  // the owner re-rasterizes the rigid bodies
    return res;
}

bool World::regionIsNew(const ShiftResult& r, int rxl, int rzl) const {
    for (const auto& p : r.loaded)
        if (p.first - originX / REGION == rxl && p.second - originZ / REGION == rzl) return true;
    return false;
}

size_t World::savedBytes() const {
    size_t n = 0;
    for (const auto& kv : savedRegions) n += kv.second.size();
    return n;
}
