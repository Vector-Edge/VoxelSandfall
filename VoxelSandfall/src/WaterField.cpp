#include "WaterField.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr float kStep = 1.0f / 60.0f;   // ripple time step
constexpr float kWaveK = 0.9f;          // (wave speed)^2 in cells^2 / step^2 * 4: ~0.45 cells per step = ~27 voxels/s
constexpr float kDamp = 0.988f;         // per step: a ripple lasts a couple of seconds
constexpr float kRestore = 0.002f;      // pulls the whole surface back towards rest (a pool is not a closed box of sloshing water)
constexpr float kMaxRipple = 1.5f;
constexpr float kFoamDecay = 0.9908f;   // per step: foam fades over ~3 s
constexpr float kQuietRipple = 0.006f, kQuietFoam = 0.02f;  // a tile below both is at rest

// float -> IEEE half (round to nearest even is not needed here; plain rounding is plenty for shading data)
uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int exp = (int)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        const int shift = 14 - exp;
        return (uint16_t)(sign | ((mant + (1u << (shift - 1))) >> shift));
    }
    if (exp >= 31) return (uint16_t)(sign | 0x7BFFu);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | ((mant + 0x1000u) >> 13));
}

}  // namespace

WaterField::WaterField() { reset(); }

void WaterField::reset() {
    const size_t n = (size_t)W * D;
    mask_.assign(n, 0);
    h_.assign(n, 0.0f);
    v_.assign(n, 0.0f);
    foam_.assign(n, 0.0f);
    smooth_.assign(n, 0.0f);
    baseSx_.assign(n, 0.0f);
    baseSz_.assign(n, 0.0f);
    shore_.assign(n, 0.0f);
    tex_.assign(n * 4, 0);
    tileAct_.assign((size_t)TXN * TZN, 0);
    tileNext_.assign((size_t)TXN * TZN, 0);
    stepAcc_ = 0;
    wet_ = 0;
    active_ = false;
    hole0X_ = hole0Z_ = 0;
    hole1X_ = hole1Z_ = -1;
    markDirty(0, 0, W - 1, D - 1);
}

void WaterField::markDirty(int x0, int z0, int x1, int z1) {
    dirty_ = true;
    dx0_ = std::min(dx0_, std::max(0, x0));
    dz0_ = std::min(dz0_, std::max(0, z0));
    dx1_ = std::max(dx1_, std::min(W - 1, x1));
    dz1_ = std::max(dz1_, std::min(D - 1, z1));
}

int WaterField::activeTiles() const {
    int n = 0;
    for (uint8_t t : tileAct_) n += t ? 1 : 0;
    return n;
}

void WaterField::activateAround(int x0, int z0, int x1, int z1) {
    const int tx0 = std::max(0, x0 / TS - 1), tx1 = std::min(TXN - 1, x1 / TS + 1);
    const int tz0 = std::max(0, z0 / TS - 1), tz1 = std::min(TZN - 1, z1 / TS + 1);
    for (int tz = tz0; tz <= tz1; tz++)
        for (int tx = tx0; tx <= tx1; tx++) tileAct_[(size_t)(tx + TXN * tz)] = 1;
    active_ = true;
}

// Re-derives everything that depends on the water surface heights inside (and around) the changed rectangle:
//   mask, smoothed height, shore proximity and the slope of the smoothed surface. The map is box-blurred with prefix sums
//   over the window, so the cost follows the size of the change, not the size of the world.
void WaterField::setSurface(const uint16_t* top, int rx0, int rz0, int rx1, int rz1) {
    // The player's dry hole is filled in at the surface level of its surroundings; the old and the new hole both count
    // as changed.
    const bool hole = exX1_ >= exX0_ && exZ1_ >= exZ0_;
    const bool hadHole = hole1X_ >= hole0X_ && hole1Z_ >= hole0Z_;
    if (hole) {
        rx0 = std::min(rx0, exX0_); rz0 = std::min(rz0, exZ0_); rx1 = std::max(rx1, exX1_); rz1 = std::max(rz1, exZ1_);
    }
    if (hadHole) {
        rx0 = std::min(rx0, hole0X_); rz0 = std::min(rz0, hole0Z_); rx1 = std::max(rx1, hole1X_); rz1 = std::max(rz1, hole1Z_);
    }
    hole0X_ = exX0_; hole0Z_ = exZ0_; hole1X_ = exX1_; hole1Z_ = exZ1_;

    // T: cells whose outputs are recomputed. S: where the smoothed height is needed. E: where the input is read.
    const int tx0 = std::max(0, rx0 - 3), tz0 = std::max(0, rz0 - 3), tx1 = std::min(W - 1, rx1 + 3), tz1 = std::min(D - 1, rz1 + 3);
    const int sx0 = std::max(0, rx0 - 4), sz0 = std::max(0, rz0 - 4), sx1 = std::min(W - 1, rx1 + 4), sz1 = std::min(D - 1, rz1 + 4);
    const int ex0 = std::max(0, rx0 - 7), ez0 = std::max(0, rz0 - 7), ex1 = std::min(W - 1, rx1 + 7), ez1 = std::min(D - 1, rz1 + 7);
    const int ew = ex1 - ex0 + 1, eh = ez1 - ez0 + 1;

    uint16_t holeTop = 0;
    auto inHole = [&](int x, int z) { return hole && x >= exX0_ && x <= exX1_ && z >= exZ0_ && z <= exZ1_; };
    if (hole)
        for (int z = std::max(0, exZ0_ - 1); z <= std::min(D - 1, exZ1_ + 1); z++)
            for (int x = std::max(0, exX0_ - 1); x <= std::min(W - 1, exX1_ + 1); x++)
                if (!inHole(x, z)) holeTop = std::max(holeTop, top[(size_t)(x + W * z)]);
    auto topAt = [&](int x, int z) -> uint16_t {
        uint16_t t = top[(size_t)(x + W * z)];
        if (!t && holeTop && inHole(x, z)) t = holeTop;
        return t;
    };

    // Prefix sums (heights and wet counts) over E: P[(z+1)*(ew+1) + (x+1)] = sum of cells < (x, z).
    static thread_local std::vector<double> ph, pc;
    const size_t pw = (size_t)ew + 1;
    ph.assign(pw * ((size_t)eh + 1), 0.0);
    pc.assign(pw * ((size_t)eh + 1), 0.0);
    for (int z = 0; z < eh; z++) {
        double rowH = 0, rowC = 0;
        for (int x = 0; x < ew; x++) {
            const uint16_t t = topAt(ex0 + x, ez0 + z);
            rowH += t;
            rowC += t ? 1.0 : 0.0;
            const size_t i = (size_t)(z + 1) * pw + (size_t)(x + 1);
            ph[i] = ph[i - pw] + rowH;
            pc[i] = pc[i - pw] + rowC;
        }
    }
    auto box = [&](const std::vector<double>& p, int x, int z, int r) {  // sum over [x-r, x+r] x [z-r, z+r], clipped to E
        const int xa = std::max(ex0, x - r) - ex0, xb = std::min(ex1, x + r) - ex0 + 1;
        const int za = std::max(ez0, z - r) - ez0, zb = std::min(ez1, z + r) - ez0 + 1;
        return p[(size_t)zb * pw + (size_t)xb] - p[(size_t)za * pw + (size_t)xb] - p[(size_t)zb * pw + (size_t)xa] + p[(size_t)za * pw + (size_t)xa];
    };

    // mask + smoothed height (R = 3: the water columns within +-3, so shore columns are not dragged towards 0)
    for (int z = tz0; z <= tz1; z++)
        for (int x = tx0; x <= tx1; x++) {
            const size_t i = (size_t)(x + W * z);
            const uint8_t m = topAt(x, z) ? 1 : 0;
            if (m != mask_[i]) {
                wet_ += m ? 1 : -1;
                mask_[i] = m;
            }
            if (!m) h_[i] = v_[i] = 0.0f;
        }
    for (int z = sz0; z <= sz1; z++)
        for (int x = sx0; x <= sx1; x++) {
            const double c = box(pc, x, z, 3);
            smooth_[(size_t)(x + W * z)] = c > 0.5 ? (float)(box(ph, x, z, 3) / c) : 0.0f;
        }
    // shore proximity (how much of the 5x5 neighbourhood is not water) and the slope of the smoothed surface
    for (int z = tz0; z <= tz1; z++)
        for (int x = tx0; x <= tx1; x++) {
            const size_t i = (size_t)(x + W * z);
            shore_[i] = mask_[i] ? 1.0f - (float)box(pc, x, z, 2) * (1.0f / 25.0f) : 0.0f;
            if (!mask_[i] || smooth_[i] <= 0.0f || x == 0 || z == 0 || x == W - 1 || z == D - 1) {
                baseSx_[i] = baseSz_[i] = 0.0f;
                continue;
            }
            const float e = smooth_[i + 1], w = smooth_[i - 1], s = smooth_[i + W], n = smooth_[i - W];
            baseSx_[i] = (e > 0 && w > 0) ? 0.5f * (e - w) : 0.0f;
            baseSz_[i] = (s > 0 && n > 0) ? 0.5f * (s - n) : 0.0f;
        }
    if (wet_ <= 0) {  // no water left anywhere
        std::fill(foam_.begin(), foam_.end(), 0.0f);
        std::fill(tileAct_.begin(), tileAct_.end(), (uint8_t)0);
        active_ = false;
    }
    repackRect(tx0, tz0, tx1, tz1);
}

void WaterField::impulse(float x, float z, float depth, float radius, float foam) {
    if (wet_ <= 0) return;
    const int cx = (int)std::floor(x), cz = (int)std::floor(z);
    const float sigma = std::max(0.6f, radius * 0.5f);
    const int reach = (int)std::ceil(sigma * 2.5f);
    const float inv = 1.0f / (2.0f * sigma * sigma);
    bool any = false;
    for (int dz = -reach; dz <= reach; dz++)
        for (int dx = -reach; dx <= reach; dx++) {
            const int px = cx + dx, pz = cz + dz;
            if (!isWater(px, pz)) continue;
            const float fx = (float)px + 0.5f - x, fz = (float)pz + 0.5f - z;
            const float w = std::exp(-(fx * fx + fz * fz) * inv);
            const size_t i = (size_t)(px + W * pz);
            h_[i] = std::clamp(h_[i] - depth * w, -kMaxRipple, kMaxRipple);
            foam_[i] = std::min(1.0f, foam_[i] + foam * w);
            any = true;
        }
    if (any) activateAround(std::max(0, cx - reach), std::max(0, cz - reach), std::min(W - 1, cx + reach), std::min(D - 1, cz + reach));
}

void WaterField::step(float dt) {
    if (!active_ || wet_ <= 0) return;
    stepAcc_ += dt;
    int steps = 0;
    // The tiles that were simulated (for the texture): everything touched during these steps.
    int ux0 = W, uz0 = D, ux1 = -1, uz1 = -1;
    while (stepAcc_ >= kStep && steps < 3) {
        stepAcc_ -= kStep;
        steps++;
        std::fill(tileNext_.begin(), tileNext_.end(), (uint8_t)0);
        // 1) velocities from the current heights
        for (int tz = 0; tz < TZN; tz++)
            for (int tx = 0; tx < TXN; tx++) {
                if (!tileAct_[(size_t)(tx + TXN * tz)]) continue;
                const int xa = std::max(1, tx * TS), xb = std::min(W - 2, tx * TS + TS - 1);
                const int za = std::max(1, tz * TS), zb = std::min(D - 2, tz * TS + TS - 1);
                for (int z = za; z <= zb; z++) {
                    const size_t row = (size_t)W * z;
                    for (int x = xa; x <= xb; x++) {
                        const size_t i = row + (size_t)x;
                        if (!mask_[i]) continue;
                        const float c = h_[i];
                        const float e = mask_[i + 1] ? h_[i + 1] : c, w = mask_[i - 1] ? h_[i - 1] : c;
                        const float s = mask_[i + W] ? h_[i + W] : c, n = mask_[i - W] ? h_[i - W] : c;
                        v_[i] = (v_[i] + (0.25f * (e + w + s + n) - c) * kWaveK - c * kRestore) * kDamp;
                    }
                }
            }
        // 2) heights and foam; a tile with nothing left in it goes to sleep (and its neighbours stop being woken by it)
        for (int tz = 0; tz < TZN; tz++)
            for (int tx = 0; tx < TXN; tx++) {
                const size_t ti = (size_t)(tx + TXN * tz);
                if (!tileAct_[ti]) continue;
                const int xa = std::max(1, tx * TS), xb = std::min(W - 2, tx * TS + TS - 1);
                const int za = std::max(1, tz * TS), zb = std::min(D - 2, tz * TS + TS - 1);
                float peak = 0, foamMax = 0;
                for (int z = za; z <= zb; z++) {
                    const size_t row = (size_t)W * z;
                    for (int x = xa; x <= xb; x++) {
                        const size_t i = row + (size_t)x;
                        if (!mask_[i]) continue;
                        const float nh = std::clamp(h_[i] + v_[i], -kMaxRipple, kMaxRipple);
                        h_[i] = nh;
                        foam_[i] *= kFoamDecay;
                        peak = std::max(peak, std::max(std::fabs(nh), std::fabs(v_[i]) * 8.0f));
                        foamMax = std::max(foamMax, foam_[i]);
                    }
                }
                ux0 = std::min(ux0, tx * TS); uz0 = std::min(uz0, tz * TS);
                ux1 = std::max(ux1, tx * TS + TS - 1); uz1 = std::max(uz1, tz * TS + TS - 1);
                if (peak >= kQuietRipple || foamMax >= kQuietFoam) {
                    for (int dz = -1; dz <= 1; dz++)
                        for (int dx = -1; dx <= 1; dx++) {
                            const int nx = tx + dx, nz = tz + dz;
                            if (nx >= 0 && nx < TXN && nz >= 0 && nz < TZN) tileNext_[(size_t)(nx + TXN * nz)] = 1;
                        }
                } else {  // at rest: clear the leftovers so the tile is exactly flat
                    for (int z = tz * TS; z < tz * TS + TS; z++) {
                        const size_t row = (size_t)W * z + (size_t)(tx * TS);
                        std::fill(h_.begin() + row, h_.begin() + row + TS, 0.0f);
                        std::fill(v_.begin() + row, v_.begin() + row + TS, 0.0f);
                        std::fill(foam_.begin() + row, foam_.begin() + row + TS, 0.0f);
                    }
                }
            }
        tileAct_.swap(tileNext_);
    }
    if (stepAcc_ > 3.0f * kStep) stepAcc_ = 0;
    if (steps && ux1 >= 0) {
        repackRect(std::max(0, ux0 - 1), std::max(0, uz0 - 1), std::min(W - 1, ux1 + 1), std::min(D - 1, uz1 + 1));
        active_ = false;
        for (uint8_t t : tileAct_) active_ |= t != 0;
    }
}

float WaterField::peakRipple() const {
    float m = 0;
    for (size_t i = 0; i < h_.size(); i++) m = std::max(m, std::fabs(h_[i]));
    return m;
}

void WaterField::repackRect(int x0, int z0, int x1, int z1) {
    x0 = std::max(0, x0);
    z0 = std::max(0, z0);
    x1 = std::min(W - 1, x1);
    z1 = std::min(D - 1, z1);
    for (int z = z0; z <= z1; z++) {
        const size_t row = (size_t)W * z;
        for (int x = x0; x <= x1; x++) {
            const size_t i = row + (size_t)x;
            uint16_t* t = &tex_[i * 4];
            if (!mask_[i]) {
                t[0] = t[1] = t[2] = t[3] = 0;
                continue;
            }
            float rx = 0, rz = 0;
            if (x > 0 && x < W - 1 && z > 0 && z < D - 1) {
                const float c = h_[i];
                const float e = mask_[i + 1] ? h_[i + 1] : c, w = mask_[i - 1] ? h_[i - 1] : c;
                const float s = mask_[i + W] ? h_[i + W] : c, n = mask_[i - W] ? h_[i - W] : c;
                rx = 0.5f * (e - w);
                rz = 0.5f * (s - n);
            }
            t[0] = toHalf(baseSx_[i] + rx);
            t[1] = toHalf(baseSz_[i] + rz);
            t[2] = toHalf(foam_[i]);
            t[3] = toHalf(shore_[i]);
        }
    }
    markDirty(x0, z0, x1, z1);
}
