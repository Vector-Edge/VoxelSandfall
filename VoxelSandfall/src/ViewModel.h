// Procedural first-person AK-style rifle (built from oriented boxes) + muzzle flash geometry.
#pragma once
#include <cstdint>
#include <vector>

#include "VoxMath.h"

struct VMVertex {
    float px, py, pz;
    float nx, ny, nz;
    uint32_t color;  // R8G8B8A8
};

struct ViewModelMesh {
    std::vector<VMVertex> verts;
    std::vector<uint32_t> idx;
    uint32_t flashFirstIndex = 0;  // indices [flashFirstIndex, end) are the additive muzzle flash
    Vec3 muzzle;                   // muzzle position in rifle space (-Z is forward)
};

ViewModelMesh buildRifle();
