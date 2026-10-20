// Dirt and grass.
//
// Dirt is "a mixture of stone and sand": loose dirt (freshly poured, or broken off) falls and piles like sand, but much
// clumpier (CellSim.cpp), and as soon as it comes to rest it becomes *static*: it keeps its shape like stone, steep walls
// and overhangs included, for as long as the structure around it can carry it. This file is the structure part:
//
//   * A static dirt/grass voxel is *supported* when an unbroken column of dirt/grass leads straight down to something solid
//     (sand, stone, bedrock, the ground). Weight travels down such a column without limit.
//   * Everything else hangs: it is held only by its sides / from above. A connected hanging mass has a number of contact
//     faces with the supported structure (and with stone), and it holds only while  mass <= kDirtStrength * contacts.
//     A big lump on a thin neck, a long thin bridge, a huge unsupported roof - they break, and the whole hanging mass
//     becomes loose dirt that falls. A hanging mass with no contact at all (cut loose) falls at once.
//
// Grass is a skin on dirt: it can only exist where there was dirt, at most two voxels deep below the open surface (tops and
// sides, not under an overhang), and it spreads (slowly) over every exposed dirt voxel in reach. Buried grass turns back into dirt. Grass is as strong as dirt
// and turns into dirt when it falls.
#include <algorithm>

#include "World.h"

namespace {
constexpr uint8_t F_KNOWN = 1, F_SUPPORTED = 2, F_VISITED = 4;
constexpr size_t kMaxHangingMass = 400000;  // beyond this a hanging mass is simply assumed to hold (it would be a whole landscape)

inline bool staticDirt(uint8_t raw) {
    const uint8_t m = raw & VOX_MASK;
    return m == M_GRASS || (m == M_DIRT && !(raw & VOX_LOOSE));
}
}  // namespace

void World::setLooseDirt(int x, int y, int z) {
    if (!inBounds(x, y, z)) return;
    setRaw(x, y, z, M_DIRT);
    data[idx(x, y, z)] |= VOX_LOOSE;
}

// How deep below the open surface is this voxel? 0 = next to air / water (on the top or a side), 1 = one voxel of soil in
// between, 2 = buried (grass can live on the first two). The underside of an overhang does not count as a surface.
int World::grassDepth(int x, int y, int z) const {
    static const int d[5][3] = {{0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    auto open = [&](int xx, int yy, int zz) {
        const uint8_t m = get(xx, yy, zz);
        return m == M_AIR || isLiquid(m);
    };
    for (const auto& k : d)
        if (open(x + k[0], y + k[1], z + k[2])) return 0;
    for (const auto& k : d) {
        const uint8_t a = get(x + k[0], y + k[1], z + k[2]);
        if ((a == M_DIRT || a == M_GRASS) && open(x + 2 * k[0], y + 2 * k[1], z + 2 * k[2])) return 1;
    }
    return 2;
}

bool World::paintGrass(int x, int y, int z) {
    if (!inBounds(x, y, z)) return false;
    const size_t i = idx(x, y, z);
    if ((data[i] & VOX_MASK) != M_DIRT || (data[i] & VOX_LOOSE)) return false;  // grass only grows on settled dirt
    if (grassDepth(x, y, z) > 1) return false;                                  // ... in a skin two voxels deep
    setRaw(x, y, z, M_GRASS);
    data[i] |= VOX_GACT;
    grassActive.push_back(packCell(x, y, z));
    markDirty(x, y, z);
    return true;
}

// Slow spreading: every so often a share of the grass that can still grow looks at the dirt around it.
void World::growGrass(int maxEntries) {
    static const int kOff[14][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 0, 1},  {0, 0, -1}, {1, 1, 0},  {-1, 1, 0}, {0, 1, 1},
                                    {0, 1, -1}, {1, -1, 0}, {-1, -1, 0}, {0, -1, 1}, {0, -1, -1}, {0, 1, 0},  {0, -1, 0}};
    int budget = maxEntries;
    while (budget-- > 0 && !grassActive.empty()) {
        if (grassCursor >= grassActive.size()) grassCursor = 0;
        int x, y, z;
        unpackCell(grassActive[grassCursor], x, y, z);
        const size_t i = idx(x, y, z);
        auto drop = [&]() {
            if ((data[i] & VOX_MASK) == M_GRASS) data[i] &= (uint8_t)~VOX_GACT;
            grassActive[grassCursor] = grassActive.back();
            grassActive.pop_back();
        };
        if ((data[i] & VOX_MASK) != M_GRASS) {  // it was dug up / replaced
            grassActive[grassCursor] = grassActive.back();
            grassActive.pop_back();
            continue;
        }
        if (grassDepth(x, y, z) >= 2) {  // buried: the grass dies back to dirt
            if (grassRng.below(100) < 50) {
                data[i] = M_DIRT;
                counts[M_GRASS]--;
                counts[M_DIRT]++;
                markDirty(x, y, z);
                grassActive[grassCursor] = grassActive.back();
                grassActive.pop_back();
                if (y > 0) {  // the grass under it is buried now as well
                    const size_t bi = idx(x, y - 1, z);
                    if ((data[bi] & VOX_MASK) == M_GRASS && !(data[bi] & VOX_GACT)) {
                        data[bi] |= VOX_GACT;
                        grassActive.push_back(packCell(x, y - 1, z));
                    }
                }
            } else {
                grassCursor++;
            }
            continue;
        }
        int cand[14], nc = 0;
        for (int k = 0; k < 14; k++) {
            const int nx = x + kOff[k][0], ny = y + kOff[k][1], nz = z + kOff[k][2];
            if (!inBounds(nx, ny, nz)) continue;
            const uint8_t nr = data[idx(nx, ny, nz)];
            if ((nr & VOX_MASK) == M_DIRT && !(nr & VOX_LOOSE) && grassDepth(nx, ny, nz) <= 1) cand[nc++] = k;
        }
        if (nc == 0) {  // nothing left to cover around here
            drop();
            continue;
        }
        if (grassRng.below(100) < 40) {
            const int k = cand[grassRng.below(nc)];
            const int nx = x + kOff[k][0], ny = y + kOff[k][1], nz = z + kOff[k][2];
            const size_t ni = idx(nx, ny, nz);
            data[ni] = (uint8_t)(M_GRASS | VOX_GACT);
            counts[M_DIRT]--;
            counts[M_GRASS]++;
            grassActive.push_back(packCell(nx, ny, nz));
            markDirty(nx, ny, nz);
        }
        grassCursor++;
    }
}

// ---- structure -------------------------------------------------------------------------------------------------
namespace {
// Does an unbroken column of static dirt / grass lead from (x, y, z) straight down to something solid? The answer is
// remembered (in `memo`) for every voxel of that column.
bool columnSupported(const World& w, std::vector<uint8_t>& memo, std::vector<size_t>& touched, int x, int y, int z) {
    static thread_local std::vector<size_t> chain;
    chain.clear();
    bool res = false;
    int yy = y;
    for (;;) {
        const size_t i = w.idx(x, yy, z);
        const uint8_t mm = memo[i];
        if (mm & F_KNOWN) {
            res = (mm & F_SUPPORTED) != 0;
            break;
        }
        chain.push_back(i);
        if (yy == 0) {  // the ground
            res = true;
            break;
        }
        const uint8_t b = w.data[w.idx(x, yy - 1, z)];
        if (staticDirt(b)) {
            yy--;
            continue;
        }
        const uint8_t bm = b & VOX_MASK;
        res = bm == M_SAND || isRigidMat(bm) || bm == M_BEDROCK;
        break;
    }
    for (size_t i : chain) {
        memo[i] |= (uint8_t)(F_KNOWN | (res ? F_SUPPORTED : 0));
        touched.push_back(i);
    }
    return res;
}
}  // namespace

// Looks at the hanging dirt around the queued voxels and drops whatever cannot hold (see the header comment).
void World::analyzeDirt(int maxSeeds) {
    if (dirtHead >= dirtQueue.size()) {
        dirtQueue.clear();
        dirtHead = 0;
        return;
    }
    if (dirtMemo.size() != data.size()) dirtMemo.assign(data.size(), 0);
    const size_t first = dirtHead, take = std::min((size_t)maxSeeds, dirtQueue.size() - dirtHead);

    static std::vector<size_t> touched;
    static std::vector<uint32_t> hanging;
    touched.clear();
    static const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

    for (size_t s = first; s < first + take; s++) {
        int x, y, z;
        unpackCell(dirtQueue[s], x, y, z);
        if (!inBounds(x, y, z)) continue;
        const size_t si = idx(x, y, z);
        if (!staticDirt(data[si]) || (dirtMemo[si] & F_VISITED)) continue;
        if (columnSupported(*this, dirtMemo, touched, x, y, z)) continue;

        // A hanging mass: collect it and count how many faces tie it to the rest.
        hanging.clear();
        hanging.push_back(dirtQueue[s]);
        dirtMemo[si] |= F_VISITED;
        touched.push_back(si);
        size_t contacts = 0;
        bool overflow = false;
        for (size_t h = 0; h < hanging.size() && !overflow; h++) {
            int cx, cy, cz;
            unpackCell(hanging[h], cx, cy, cz);
            for (const auto& d : nb) {
                const int nx = cx + d[0], ny = cy + d[1], nz = cz + d[2];
                if (!inBounds(nx, ny, nz)) continue;
                const size_t ni = idx(nx, ny, nz);
                const uint8_t nr = data[ni];
                if (staticDirt(nr)) {
                    if (columnSupported(*this, dirtMemo, touched, nx, ny, nz)) {
                        contacts++;
                    } else if (!(dirtMemo[ni] & F_VISITED)) {
                        dirtMemo[ni] |= F_VISITED;
                        touched.push_back(ni);
                        hanging.push_back(packCell(nx, ny, nz));
                        if (hanging.size() > kMaxHangingMass) overflow = true;
                    }
                } else {
                    const uint8_t nm = nr & VOX_MASK;
                    if (nm == M_SAND || isRigidMat(nm) || nm == M_BEDROCK) contacts++;  // stuck to a wall / ceiling
                }
            }
        }
        if (overflow) continue;
        if (contacts != 0 && hanging.size() <= (size_t)kDirtStrength * contacts) continue;  // it holds

        // It breaks: the whole hanging mass lets go.
        for (uint32_t c : hanging) {
            int hx, hy, hz;
            unpackCell(c, hx, hy, hz);
            const size_t hi = idx(hx, hy, hz);
            if ((data[hi] & VOX_MASK) == M_GRASS) {
                counts[M_GRASS]--;
                counts[M_DIRT]++;
            }
            data[hi] = (uint8_t)(M_DIRT | VOX_LOOSE);
            markDirty(hx, hy, hz);
            queueCell(hx, hy, hz);
        }
        dirtCollapsed += (uint32_t)hanging.size();
    }
    for (size_t i : touched) dirtMemo[i] = 0;
    dirtHead += take;
    if (dirtHead >= dirtQueue.size()) {
        dirtQueue.clear();
        dirtHead = 0;
    } else if (dirtHead > 65536) {
        dirtQueue.erase(dirtQueue.begin(), dirtQueue.begin() + (std::ptrdiff_t)dirtHead);
        dirtHead = 0;
    }
}
