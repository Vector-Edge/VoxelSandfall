// Makes the water surface lively without touching the voxel grid: a 2D field over the world's (x, z) columns holding
//   - ripples: a damped wave equation that splashes, droplets and the wading player disturb,
//   - foam: left behind by splashes, fades away,
//   - a smoothed copy of the (integer) voxel surface height, so the stair-stepped voxel surface shades as a smooth one,
//   - shore proximity (for the foam line where water meets sand or stone).
// The renderer feeds it the surface heights of the open water and uploads texels() as a RGBA16F texture:
//   R,G = surface slope (d height / d x, d height / d z) = smoothed voxel surface + ripples, B = foam, A = shore.
//
// Everything is incremental: the wave simulation only runs on 16x16 tiles that hold ripples or foam (or touch such a
// tile), the surface is only re-smoothed where it changed, and only the changed rectangle of the texture is uploaded.
#pragma once
#include <cstdint>
#include <vector>

#include "Config.h"

class WaterField {
public:
    static constexpr int W = cfg::WORLD_X, D = cfg::WORLD_Z;
    static constexpr int TS = 16;  // simulation tile size (columns)
    static constexpr int TXN = W / TS, TZN = D / TS;

    WaterField();
    void reset();

    // surfaceTop[x + W * z] = height of the open water surface in that column (the y of its top plane), 0 = none.
    // [x0..x1] x [z0..z1] (inclusive) is the part of the map that changed since the last call (default: all of it).
    void setSurface(const uint16_t* surfaceTop, int x0 = 0, int z0 = 0, int x1 = W - 1, int z1 = D - 1);
    // The (inclusive) column box the player keeps free of water: for the field it counts as water at the level of its
    // surroundings, so the body does not show up as a dry square with a shore line around it. Applied by setSurface().
    void setExclusion(int x0, int z0, int x1, int z1) {
        exX0_ = x0; exZ0_ = z0; exX1_ = x1; exZ1_ = z1;
    }
    void clearExclusion() { exX0_ = exZ0_ = 0; exX1_ = exZ1_ = -1; }
    bool hasWater() const { return wet_ > 0; }
    bool active() const { return active_; }
    bool isWater(int x, int z) const { return (unsigned)x < (unsigned)W && (unsigned)z < (unsigned)D && mask_[(size_t)(x + W * z)] != 0; }

    // A drop or impact: presses the surface down around (x, z) (a Gaussian of the given radius, in voxels) and
    // leaves foam. Ignored where there is no open water.
    void impulse(float x, float z, float depth, float radius, float foam = 0.0f);

    // Advances the ripples (fixed internal step) and fades the foam.
    void step(float dt);

    // The world window moved: new[x][z] = old[x + dx][z + dz] (columns; both multiples of the 16 column tile size).
    void shift(int dx, int dz);

    // W * D * 4 half floats. consumeDirty() returns the (inclusive) rectangle of columns that changed since last asked.
    const uint16_t* texels() const { return tex_.data(); }
    bool consumeDirty(int& x0, int& z0, int& x1, int& z1) {
        if (!dirty_) return false;
        x0 = dx0_; z0 = dz0_; x1 = dx1_; z1 = dz1_;
        clearDirty();
        return true;
    }
    bool consumeDirty() {
        int a, b, c, d;
        return consumeDirty(a, b, c, d);
    }
    int activeTiles() const;

    // Largest ripple height (voxels) at the moment, for tests.
    float peakRipple() const;
    float rippleAt(int x, int z) const { return h_[(size_t)(x + W * z)]; }
    float foamAt(int x, int z) const { return foam_[(size_t)(x + W * z)]; }
    float shoreAt(int x, int z) const { return shore_[(size_t)(x + W * z)]; }
    float smoothAt(int x, int z) const { return smooth_[(size_t)(x + W * z)]; }

private:
    void repackRect(int x0, int z0, int x1, int z1);  // rebuilds tex_ for a rectangle and marks it dirty
    void markDirty(int x0, int z0, int x1, int z1);
    void clearDirty() {
        dirty_ = false;
        dx0_ = W;
        dz0_ = D;
        dx1_ = dz1_ = -1;
    }
    void activateAround(int x0, int z0, int x1, int z1);  // wake the tiles under a rectangle (and their neighbours)

    std::vector<uint8_t> mask_;                 // 1 where the column has open water
    std::vector<float> h_, v_, foam_;           // ripple height, velocity, foam
    std::vector<float> smooth_;                 // blurred surface height
    std::vector<float> baseSx_, baseSz_, shore_;
    std::vector<uint16_t> tex_;
    std::vector<uint8_t> tileAct_, tileNext_;   // tiles being simulated
    float stepAcc_ = 0;
    int exX0_ = 0, exZ0_ = 0, exX1_ = -1, exZ1_ = -1;     // current exclusion (player) box
    int hole0X_ = 0, hole0Z_ = 0, hole1X_ = -1, hole1Z_ = -1;  // the box as of the last setSurface()
    int dx0_ = W, dz0_ = D, dx1_ = -1, dz1_ = -1;         // dirty texture rectangle
    long long wet_ = 0;
    bool dirty_ = false, active_ = false;
};
