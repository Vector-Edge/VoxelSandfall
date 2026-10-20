// Burning coal and oil in the voxel grid.
//
//   Coal    - when a flame (or a burning neighbour) lights it, it becomes burning coal and stays on fire for 25 - 70 s, then
//             it is gone: a lump burns away voxel by voxel. A burning voxel lights the coal next to it a third less
//             eagerly than a flame does, so a fire creeps through a clump rather than flashing over it. Coal with water
//             next to it cannot catch fire, and burning coal that water reaches goes out.
//   Oil     - catches fire where a flame touches it, and the fire sweeps through all the oil connected to it as a wave (40 - 320
//             voxels a second, by the amount of oil: any pool is alight in a second or two; two places lit are two waves that meet in the
//             middle), the fresh front flaring up higher than what has been burning for a while. Each oil voxel burns for
//             10 - 30 s and is gone. It lights the coal it touches at once. Burning oil does not flow (it stays where it
//             burns; it falls if the ground under it burns away); oil under water cannot burn, and water poured over
//             burning oil puts it out.
#include <algorithm>
#include <cmath>

#include "Fire.h"
#include "World.h"

namespace {
constexpr int kMaxWaveLayers = 40;     // oil layers the wave may cross in one step (a very big lake does not stall)
constexpr size_t kMaxVisits = 60000;  // burning voxels looked at per step; more are visited in turns (and age faster per visit)
}  // namespace

bool World::wet(int x, int y, int z) const {
    return get(x + 1, y, z) == M_WATER || get(x - 1, y, z) == M_WATER || get(x, y + 1, z) == M_WATER || get(x, y - 1, z) == M_WATER ||
           get(x, y, z + 1) == M_WATER || get(x, y, z - 1) == M_WATER;
}

void World::registerBurning(int x, int y, int z, float left) {
    if (left < 0) left = get(x, y, z) == M_BOIL ? fireRng.range(fire::kOilBurnMin, fire::kOilBurnMax) : fireRng.range(fire::kCoalBurnMin, fire::kCoalBurnMax);
    burning.push_back({packCell(x, y, z), left});
}

bool World::ignite(int x, int y, int z, BurnReport* rep) {
    if (!inBounds(x, y, z)) return false;
    const size_t i = idx(x, y, z);
    const uint8_t m = data[i] & VOX_MASK;
    if (m == M_COAL) {
        if (wet(x, y, z)) return false;
        data[i] = M_BCOAL;
        counts[M_COAL]--;
        counts[M_BCOAL]++;
        markDirty(x, y, z);
        burning.push_back({packCell(x, y, z), fireRng.range(fire::kCoalBurnMin, fire::kCoalBurnMax)});
        if (rep) rep->caught.push_back(packCell(x, y, z));
        return true;
    }
    return lightOil(x, y, z, rep, oilFront);  // (the wave carries it on from here)
}

bool World::lightOil(int x, int y, int z, BurnReport* rep, std::vector<uint32_t>& front) {
    if (get(x, y, z) != M_OIL || get(x, y + 1, z) == M_WATER) return false;
    data[idx(x, y, z)] = M_BOIL;
    counts[M_OIL]--;
    counts[M_BOIL]++;
    markDirtyWater(x, y, z);
    const uint32_t c = packCell(x, y, z);
    burning.push_back({c, fireRng.range(fire::kOilBurnMin, fire::kOilBurnMax)});
    front.push_back(c);
    if (rep) rep->caught.push_back(c);
    return true;
}

// The front moves on through the oil, kOilWaveSpeed layers a second: every voxel of it lights the oil voxels around it.
void World::advanceOilWave(float dt, BurnReport& rep) {
    if (oilFront.empty()) {
        oilWaveAcc = 0;
        return;
    }
    const float speed = std::clamp(fire::kOilWaveScale * std::sqrt((float)(counts[M_OIL] + counts[M_BOIL])), fire::kOilWaveMinSpeed, fire::kOilWaveMaxSpeed);
    oilWaveAcc += speed * dt;
    const int layers = std::min((int)oilWaveAcc, kMaxWaveLayers);
    oilWaveAcc -= (float)(int)oilWaveAcc;
    static thread_local std::vector<uint32_t> next;
    static const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int l = 0; l < layers && !oilFront.empty(); l++) {
        next.clear();
        for (uint32_t c : oilFront) {
            int x, y, z;
            unpackCell(c, x, y, z);
            for (const auto& d : nb) lightOil(x + d[0], y + d[1], z + d[2], &rep, next);
        }
        oilFront.swap(next);
    }
}

void World::burnStep(float dt, BurnReport& rep) {
    advanceOilWave(dt, rep);
    const size_t total = burning.size();
    if (!total) {
        burnCursor = 0;
        return;
    }
    const size_t visits = std::min(total, kMaxVisits);
    const float scale = (float)total / (float)visits;  // an entry that is visited less often ages faster when it is
    size_t i = burnCursor >= total ? 0 : burnCursor;
    const size_t begin = i;
    size_t end = std::min(total, begin + visits);
    const float step = dt * scale;
    const float pSpreadCoal = std::min(1.0f, fire::kFlameLightsCoal * fire::kSpreadFactor * step);
    static const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

    while (i < end && i < burning.size()) {
        const uint32_t cell = burning[i].cell;
        float left = burning[i].left - step;
        int x, y, z;
        unpackCell(cell, x, y, z);
        const uint8_t m = get(x, y, z);
        bool remove = false;
        if (m == M_BCOAL) {
            if (wet(x, y, z)) {  // water has reached it: it goes out (and stays coal)
                const size_t di = idx(x, y, z);
                data[di] = M_COAL;
                counts[M_BCOAL]--;
                counts[M_COAL]++;
                markDirty(x, y, z);
                rep.quenched.push_back(cell);
                remove = true;
            } else if (left <= 0) {  // burnt away
                data[idx(x, y, z)] = M_AIR;
                counts[M_BCOAL]--;
                counts[M_AIR]++;
                markDirty(x, y, z);
                wakeAround(x, y, z);
                rep.burntOut.push_back(cell);
                remove = true;
            } else {
                for (const auto& d : nb) {
                    const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
                    const uint8_t nm = get(nx, ny, nz);
                    if (nm == M_COAL) {
                        if (fireRng.f() < pSpreadCoal) ignite(nx, ny, nz, &rep);
                    } else if (nm == M_OIL) {
                        lightOil(nx, ny, nz, &rep, oilFront);  // oil catches fire the moment anything burning touches it
                    }
                }
            }
        } else if (m == M_BOIL) {
            if (get(x, y + 1, z) == M_WATER) {  // water over it: out (it is oil again and flows)
                data[idx(x, y, z)] = M_OIL;
                counts[M_BOIL]--;
                counts[M_OIL]++;
                markDirtyWater(x, y, z);
                queueCell(x, y, z);
                rep.quenched.push_back(cell);
                remove = true;
            } else if (left <= 0) {
                data[idx(x, y, z)] = M_AIR;
                counts[M_BOIL]--;
                counts[M_AIR]++;
                markDirtyWater(x, y, z);
                wakeAround(x, y, z);
                rep.burntOut.push_back(cell);
                remove = true;
            } else {
                int ny = y;  // the ground under it burnt away: it drops
                while (y - ny < 3 && cellFree(x, ny - 1, z)) ny--;
                if (ny != y) {
                    data[idx(x, ny, z)] = M_BOIL;
                    data[idx(x, y, z)] = M_AIR;
                    markDirtyWater(x, y, z);
                    markDirtyWater(x, ny, z);
                    wakeAround(x, y, z);
                    y = ny;
                    burning[i].cell = packCell(x, y, z);
                }
                for (const auto& d : nb) {
                    const int nx = x + d[0], ny2 = y + d[1], nz = z + d[2];
                    const uint8_t nm = get(nx, ny2, nz);
                    if (nm == M_COAL) ignite(nx, ny2, nz, &rep);  // burning oil lights everything it touches
                    else if (nm == M_OIL && fireRng.f() < 0.04f) lightOil(nx, ny2, nz, &rep, oilFront);  // (a safety net: oil poured against a fire later)
                }
            }
        } else {
            remove = true;  // not what it was (carved away, replaced)
        }
        if (remove) {
            burning[i] = burning.back();
            burning.pop_back();
            end = std::min(end, burning.size());
        } else {
            burning[i].left = left;
            burning[i].age += step;
            i++;
        }
    }
    burnCursor = (visits >= total || i >= burning.size()) ? 0 : i;  // (entries added during the step sit at the end: they are not skipped)
}
