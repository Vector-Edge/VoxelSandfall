#include "World.h"

#include "Parallel.h"

World::World() {
    size_t total = (size_t)chunkCount() << 15;
    data.assign(total, 0);
    overlay.assign(total, 0);
    meshDirty.assign(chunkCount(), 1);
    texDirty.assign(chunkCount(), 1);
    waterDirty.assign(chunkCount(), 0);
    brickSolid.assign((size_t)chunkCount() * 64, 0);
    summaryDirty.assign(chunkCount(), 1);
    counts[0] = (long long)total;
    tileCur.resize((size_t)TX * TZ);
    tileNext.resize((size_t)TX * TZ);
    tileRng.resize((size_t)TX * TZ);
    for (int i = 0; i < TX * TZ; i++) tileRng[(size_t)i].r = Rng(0x9E3779B9u ^ (uint32_t)(i * 2654435761u));
    simWorkers.resize((size_t)globalPool().participants());
}

void World::markAllDirty() {
    std::fill(summaryDirty.begin(), summaryDirty.end(), (uint8_t)1);
    std::fill(meshDirty.begin(), meshDirty.end(), (uint8_t)1);
    std::fill(texDirty.begin(), texDirty.end(), (uint8_t)1);
}

void World::markDirty(int x, int y, int z) { markDirtyImpl(x, y, z, meshDirty); }
void World::markDirtyWater(int x, int y, int z) { markDirtyImpl(x, y, z, waterDirty); }

void World::markDirtyImpl(int x, int y, int z, std::vector<uint8_t>& meshFlags) {
    int lx = x & 31, ly = y & 31, lz = z & 31;
    int cx = x >> 5, cy = y >> 5, cz = z >> 5;
    // Flags are tested before they are written: the sim threads hit the same few chunks constantly and
    // unconditional stores would bounce those cache lines between cores.
    const int own = chunkIndex(cx, cy, cz);
    if (&meshFlags == &meshDirty && !summaryDirty[own]) summaryDirty[own] = 1;  // solids may have changed (not for water <-> air)
    if (!texDirty[own]) texDirty[own] = 1;
    if ((unsigned)(lx - 1) < 30u && (unsigned)(ly - 1) < 30u && (unsigned)(lz - 1) < 30u) {  // interior: own chunk only
        if (!meshFlags[own]) meshFlags[own] = 1;
        return;
    }
    int xa = lx == 0 ? -1 : 0, xb = lx == 31 ? 1 : 0;
    int ya = ly == 0 ? -1 : 0, yb = ly == 31 ? 1 : 0;
    int za = lz == 0 ? -1 : 0, zb = lz == 31 ? 1 : 0;
    for (int dz = za; dz <= zb; dz++)
        for (int dy = ya; dy <= yb; dy++)
            for (int dx = xa; dx <= xb; dx++) {
                int ax = cx + dx, ay = cy + dy, az = cz + dz;
                if (ax < 0 || ay < 0 || az < 0 || ax >= CX || ay >= CY || az >= CZ) continue;
                const int ni = chunkIndex(ax, ay, az);
                if (!meshFlags[ni]) meshFlags[ni] = 1;
            }
}

bool World::anySolidIn(int x0, int y0, int z0, int x1, int y1, int z1) const {
    if (y0 < 0) return true;  // the ground
    x0 = std::max(x0, 0); z0 = std::max(z0, 0);
    x1 = std::min(x1, SX - 1); z1 = std::min(z1, SZ - 1);
    y1 = std::min(y1, SY - 1);
    if (x0 > x1 || y0 > y1 || z0 > z1) return false;
    for (int bz = z0 >> 3; bz <= (z1 >> 3); bz++)
        for (int by = y0 >> 3; by <= (y1 >> 3); by++)
            for (int bx = x0 >> 3; bx <= (x1 >> 3); bx++) {
                const int ci = chunkIndex(bx >> 2, by >> 2, bz >> 2);
                uint8_t* flags = &brickSolid[(size_t)ci * 64];
                if (summaryDirty[ci]) {
                    const uint8_t* c = chunkData(ci);
                    std::fill(flags, flags + 64, (uint8_t)0);
                    for (int z = 0; z < 32; z++)
                        for (int y = 0; y < 32; y++) {
                            const uint8_t* row = c + (z << 10) + (y << 5);
                            uint8_t* f = flags + ((y >> 3) << 2) + ((z >> 3) << 4);
                            for (int b = 0; b < 4; b++) {
                                uint8_t any = 0;
                                for (int x = 0; x < 8; x++) {
                                    const uint8_t m = row[b * 8 + x] & VOX_MASK;
                                    any |= (m == M_SAND || m >= M_STONE);
                                }
                                f[b] |= any;
                            }
                        }
                    summaryDirty[ci] = 0;
                }
                if (flags[(bx & 3) + 4 * (by & 3) + 16 * (bz & 3)]) return true;
            }
    return false;
}

void World::queueCell(int x, int y, int z) {
    if (!inBounds(x, y, z)) return;
    size_t i = idx(x, y, z);
    uint8_t v = data[i];
    if (isCellular(v & VOX_MASK) && !(v & VOX_QUEUED)) {
        data[i] = v | VOX_QUEUED;
        tileNext[(size_t)tileOf(x, z)].v.push_back(packCell(x, y, z));
    }
}

void World::wakeAround(int x, int y, int z) {
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) queueCell(x + dx, y + dy, z + dz);
}

void World::set(int x, int y, int z, uint8_t m) {
    if (!inBounds(x, y, z)) return;
    size_t i = idx(x, y, z);
    uint8_t old = data[i] & VOX_MASK;
    if (old == m) return;
    data[i] = m;
    counts[old]--;
    counts[m]++;
    if ((old == M_WATER && m == M_AIR) || (old == M_AIR && m == M_WATER)) markDirtyWater(x, y, z);
    else markDirty(x, y, z);
    wakeAround(x, y, z);
}

size_t World::activeCount() const {
    size_t n = 0;
    for (const auto& t : tileNext) n += t.v.size();
    return n;
}

void World::setRaw(int x, int y, int z, uint8_t m) {
    if (!inBounds(x, y, z)) return;
    size_t i = idx(x, y, z);
    uint8_t old = data[i] & VOX_MASK;
    if (old == m) return;
    data[i] = m;
    counts[old]--;
    counts[m]++;
}

void World::notifyBox(int x0, int y0, int z0, int x1, int y1, int z1, bool waterOnly) {
    x0 = std::max(x0 - 1, 0); y0 = std::max(y0 - 1, 0); z0 = std::max(z0 - 1, 0);
    x1 = std::min(x1 + 1, SX - 1); y1 = std::min(y1 + 1, SY - 1); z1 = std::min(z1 + 1, SZ - 1);
    for (int cz = z0 >> 5; cz <= (z1 >> 5); cz++)
        for (int cy = y0 >> 5; cy <= (y1 >> 5); cy++)
            for (int cx = x0 >> 5; cx <= (x1 >> 5); cx++) {
                int ci = chunkIndex(cx, cy, cz);
                (waterOnly ? waterDirty : meshDirty)[ci] = 1;
                if (!waterOnly) summaryDirty[ci] = 1;
                texDirty[ci] = 1;
            }
    // Wake every cellular voxel in (and one cell around) the box. Rows are contiguous in x inside a chunk,
    // so this is a cheap linear scan.
    for (int z = z0; z <= z1; z++)
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                uint8_t v = data[idx(x, y, z)];
                if (isCellular(v & VOX_MASK) && !(v & VOX_QUEUED)) queueCell(x, y, z);
            }
}

void World::notifyBoxWater(int x0, int y0, int z0, int x1, int y1, int z1) {
    const int ax0 = std::max(x0 - 1, 0), ay0 = std::max(y0 - 1, 0), az0 = std::max(z0 - 1, 0);
    const int ax1 = std::min(x1 + 1, SX - 1), ay1 = std::min(y1 + 1, SY - 1), az1 = std::min(z1 + 1, SZ - 1);
    for (int cz = az0 >> 5; cz <= (az1 >> 5); cz++)
        for (int cy = ay0 >> 5; cy <= (ay1 >> 5); cy++)
            for (int cx = ax0 >> 5; cx <= (ax1 >> 5); cx++) {
                const int ci = chunkIndex(cx, cy, cz);
                waterDirty[ci] = 1;
                texDirty[ci] = 1;
            }
    // the shell just outside the box
    for (int z = az0; z <= az1; z++)
        for (int y = ay0; y <= ay1; y++) {
            const bool zEdge = z == az0 || z == az1, yEdge = y == ay0 || y == ay1;
            if (zEdge || yEdge) {
                for (int x = ax0; x <= ax1; x++) queueCell(x, y, z);
            } else {
                queueCell(ax0, y, z);
                queueCell(ax1, y, z);
            }
        }
}

void World::setWaterQuiet(int x, int y, int z) {
    if (!inBounds(x, y, z)) return;
    const size_t i = idx(x, y, z);
    if ((data[i] & VOX_MASK) != M_AIR) return;
    data[i] = M_WATER;
    counts[M_AIR]--;
    counts[M_WATER]++;
    markDirtyWater(x, y, z);
    queueCell(x, y - 1, z);
    if (((++quietCounter) & 7u) == 0) queueCell(x, y, z);
}

void World::generateArena() {
    std::fill(data.begin(), data.end(), (uint8_t)0);
    std::fill(overlay.begin(), overlay.end(), (uint8_t)0);
    overlayActive = false;
    for (auto& t : tileCur) t.v.clear();
    for (auto& t : tileNext) t.v.clear();
    dirtQueue.clear();
    dirtHead = 0;
    grassActive.clear();
    grassCursor = 0;
    auto put = [&](int x, int y, int z, uint8_t m) { data[idx(x, y, z)] = m; };

    // 256 x 256 x 1 Permanent Stone (bedrock) floor.
    for (int z = cfg::FLOOR_Z0; z < cfg::FLOOR_Z1; z++)
        for (int x = cfg::FLOOR_X0; x < cfg::FLOOR_X1; x++) put(x, 0, z, M_BEDROCK);

    // Perimeter walls of Normal Stone, 3 voxels thick, enclosing the floor on all four sides.
    const int x0 = cfg::ARENA_OX, x1 = cfg::ARENA_OX + cfg::ARENA_TOTAL;
    const int z0 = cfg::ARENA_OZ, z1 = cfg::ARENA_OZ + cfg::ARENA_TOTAL;
    for (int y = 0; y < cfg::WALL_HEIGHT; y++)
        for (int z = z0; z < z1; z++)
            for (int x = x0; x < x1; x++) {
                bool inner = x >= cfg::FLOOR_X0 && x < cfg::FLOOR_X1 && z >= cfg::FLOOR_Z0 && z < cfg::FLOOR_Z1;
                if (!inner) put(x, y, z, M_STONE);
            }

    for (auto& c : counts) c = 0;
    for (size_t i = 0; i < data.size(); i++) counts[data[i] & VOX_MASK]++;
    markAllDirty();
}

RayHit World::raycast(const Vec3& o, const Vec3& d, float maxDist, uint32_t mask) const {
    RayHit r;
    const float dims[3] = {(float)SX, (float)SY, (float)SZ};
    float t0 = 0, t1 = maxDist;
    for (int a = 0; a < 3; a++) {
        if (std::fabs(d[a]) < 1e-9f) {
            if (o[a] < 0 || o[a] >= dims[a]) return r;
        } else {
            float tn = (0 - o[a]) / d[a], tf = (dims[a] - o[a]) / d[a];
            if (tn > tf) std::swap(tn, tf);
            t0 = std::max(t0, tn);
            t1 = std::min(t1, tf);
            if (t0 > t1) return r;
        }
    }
    Vec3 p = o + d * (t0 + 1e-4f);
    int cell[3] = {std::clamp(floorToInt(p.x), 0, SX - 1), std::clamp(floorToInt(p.y), 0, SY - 1),
                   std::clamp(floorToInt(p.z), 0, SZ - 1)};
    int step[3];
    float tMax[3], tDelta[3];
    for (int a = 0; a < 3; a++) {
        if (d[a] > 0) {
            step[a] = 1;
            tDelta[a] = 1.0f / d[a];
            tMax[a] = t0 + ((cell[a] + 1) - p[a]) * tDelta[a];
        } else if (d[a] < 0) {
            step[a] = -1;
            tDelta[a] = -1.0f / d[a];
            tMax[a] = t0 + (p[a] - cell[a]) * tDelta[a];
        } else {
            step[a] = 0;
            tDelta[a] = 1e30f;
            tMax[a] = 1e30f;
        }
    }
    IVec3 normal(0, 0, 0);
    // If we start inside the grid at t0 == 0 there is no entry face; use the dominant axis.
    {
        int ax = 0;
        float best = 0;
        for (int a = 0; a < 3; a++)
            if (std::fabs(d[a]) > best) { best = std::fabs(d[a]); ax = a; }
        normal[ax] = d[ax] > 0 ? -1 : 1;
    }
    float tEnter = t0;
    const int maxSteps = SX + SY + SZ + 8;
    for (int s = 0; s < maxSteps; s++) {
        uint8_t m = data[idx(cell[0], cell[1], cell[2])] & VOX_MASK;
        if (mask & matBit(m)) {
            r.hit = true;
            r.t = tEnter;
            r.point = o + d * tEnter;
            r.normal = normal;
            r.cell = IVec3(cell[0], cell[1], cell[2]);
            return r;
        }
        int a = 0;
        if (tMax[1] < tMax[a]) a = 1;
        if (tMax[2] < tMax[a]) a = 2;
        if (tMax[a] > t1) return r;
        tEnter = tMax[a];
        cell[a] += step[a];
        tMax[a] += tDelta[a];
        normal = IVec3(0, 0, 0);
        normal[a] = -step[a];
        if (cell[a] < 0 || cell[a] >= (a == 0 ? SX : a == 1 ? SY : SZ)) return r;
    }
    return r;
}
