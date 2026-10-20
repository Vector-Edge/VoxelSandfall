// Fire, smoke and embers. Flames, smoke and sparks are particles (they move freely, glow, and are drawn as soft sprites); coal
// and oil that burn are voxels that carry on burning where they are (World::burning, FireWorld.cpp). Flames are what sets
// things alight: a flame that touches coal, oil, or a coal rock lights it. The game side is GameFire.cpp.
#pragma once
#include "VoxMath.h"

namespace fire {
constexpr float kStepHz = 30.0f;             // how often burn timers, ignition and spreading are advanced
constexpr float kFlameLightsCoal = 0.38f;    // per second: the chance rate with which a flame touching coal sets it alight
constexpr float kSpreadFactor = 2.0f / 3.0f; // burning coal lights the coal next to it a third less eagerly than a flame does
constexpr float kCoalBurnMin = 25.0f, kCoalBurnMax = 70.0f;  // seconds a coal voxel burns before it is gone
constexpr float kOilBurnMin = 10.0f, kOilBurnMax = 30.0f;    // seconds an oil voxel burns before it is gone
// How fast fire sweeps through oil from where it was lit (voxels a second): slow enough to see as a wave in a small puddle, faster
// for a bigger pool so that any pool is alight in a second or two (speed ~ 0.8 x the square root of the oil, within these limits).
constexpr float kOilWaveMinSpeed = 40.0f, kOilWaveMaxSpeed = 320.0f, kOilWaveScale = 0.80f;
constexpr int kMaxParticles = 30000;
constexpr int kMaxLights = 16;
}  // namespace fire

enum FxKind : uint8_t {
    FX_FLAME = 0,  // a lick of flame: rises, shrinks, glows (additive)
    FX_EMBER = 1,  // a spark: small, bright, flung up and out, falls back
    FX_SMOKE = 2,  // grey smoke (burning coal, spent flames): drifts up slowly and spreads
    FX_STEAM = 3,  // pale smoke where fire met water
    FX_SOOT = 4    // dark smoke (burning oil)
};

struct FxParticle {
    Vec3 pos, vel;
    float age = 0, life = 1;  // seconds
    float size = 6;           // at birth (flames shrink, smoke grows)
    float strength = 1;       // flames: how hot / big (burning oil is more)
    float seed = 0;           // per particle random in [0, 1)
    uint8_t kind = FX_FLAME;
};

// A glow the shaders add to nearby surfaces: where a good deal of flame is (a cluster of flames, merged).
struct FireLight {
    Vec3 pos;
    float radius = 60;  // voxels
    float power = 1;
};
