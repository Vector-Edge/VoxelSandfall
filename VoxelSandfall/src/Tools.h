// Creator / Gun tools, destruction and dynamic island splitting.
#pragma once
#include <vector>

#include "VoxMath.h"
#include "Physics.h"
#include "World.h"

// Material that the Creator tool paints (index into kPaintMaterials).
constexpr uint8_t kPaintMaterials[4] = {M_SAND, M_WATER, M_STONE, M_BEDROCK};
const char* materialName(uint8_t m);

struct SceneHit {
    bool hit = false;
    float t = 0;
    Vec3 point;
    Vec3 normal;
    Body* body = nullptr;  // non-null when a rigid body was hit
    IVec3 cell;
    uint8_t material = M_AIR;
};

// Nearest hit against the voxel grid and all rigid bodies. `mask` selects hittable grid materials.
SceneHit castScene(const World& w, const Physics& ph, const Vec3& origin, const Vec3& dir, float maxDist, uint32_t mask);

struct CarveResult {
    int removedWorld = 0;
    int removedBody = 0;
    int islandsCreated = 0;
    int islandVoxels = 0;
    std::vector<Vec3> debris;  // sample positions of removed voxels (for particles)
    std::vector<uint8_t> debrisMat;
};

// Destroys every non-bedrock voxel inside the sphere (world grid + rigid bodies) and runs the
// 3D flood-fill island detection; detached stone becomes new rigid bodies.
CarveResult carveSphere(World& w, Physics& ph, const Vec3& center, float radius, Rng& rng);

// Detects unanchored Normal Stone islands touching the given seed cells and converts them to bodies.
int detachIslands(World& w, Physics& ph, const std::vector<IVec3>& seeds, int* voxelsMoved = nullptr);

struct PaintResult {
    int placed = 0;
    int staticStone = 0;
    int newBodies = 0;
    int mergedIntoBody = 0;
};

// Creator tool: paints `material` into all legal cells of the sphere. `blockedBox` (min,max) is the
// player's AABB, which is kept free of solid voxels.
PaintResult paintSphere(World& w, Physics& ph, const Vec3& center, float radius, uint8_t material,
                        const Vec3& playerMin, const Vec3& playerMax);
