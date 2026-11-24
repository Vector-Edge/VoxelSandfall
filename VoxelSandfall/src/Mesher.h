// Voxel meshing: greedy (merged) quads for the world chunks, per-vertex AO quads for rigid bodies.
#pragma once
#include <vector>

#include "Physics.h"
#include "VoxMath.h"
#include "World.h"

// Vertex layout (matches the ATTR input in world.hlsl): attr bits 0-2 face, 3-6 material, 7-8 AO.
struct MeshVertex {
    float x, y, z;
    uint32_t attr;
};

struct MeshData {
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    void clear() {
        verts.clear();
        idx.clear();
    }
    bool empty() const { return idx.empty(); }
};

// Opaque (sand / stone / bedrock) and water faces of one 32^3 chunk, in world coordinates. Thread-safe
// (reads the world only); ambient occlusion for these meshes is computed per pixel from the occupancy volume.
void buildChunkMesh(const World& w, int cx, int cy, int cz, MeshData& opaque, MeshData& water);

// Only the water faces of a chunk (for chunks where just water moved). Thread-safe.
void buildChunkWaterMesh(const World& w, int cx, int cy, int cz, MeshData& water);

// Per-column map of the open water in a chunk, read from its water mesh: out[x + 32 * z] = height (1..32, local) of the
// highest water surface plane in that column, 0 = none. Thread-safe.
void waterTopsOfMesh(const MeshData& water, int cx, int cy, int cz, uint8_t out[1024]);

// 64 bytes: occupancy of the chunk's 4x4x4 bricks of 8^3 voxels (index bx + 4*(by + 4*bz)). Thread-safe.
void computeBricks(const World& w, int chunkIndex, uint8_t out[64]);

// Faces of a rigid body in its lattice space (the renderer applies the body transform).
void buildBodyMesh(const Body& b, MeshData& opaque);

// Axis-aligned unit cubes (debris), centred on `c` with edge length `size`.
void appendCube(MeshData& m, const Vec3& c, float size, uint8_t material);
