// Voxel world: chunk-contiguous byte grid, cellular-automaton state, ray casting.
#pragma once
#include <vector>

#include "Config.h"
#include "VoxMath.h"

enum Material : uint8_t { M_AIR = 0, M_SAND = 1, M_WATER = 2, M_STONE = 3, M_BEDROCK = 4 };

inline bool isCellular(uint8_t m) { return m == M_SAND || m == M_WATER; }
inline bool isOpaque(uint8_t m) { return m == M_SAND || m >= M_STONE; }   // blocks light / the player
inline bool isRigidSolid(uint8_t m) { return m == M_SAND || m >= M_STONE; }  // what rigid bodies collide with
inline uint32_t matBit(uint8_t m) { return 1u << m; }

// Raw voxel byte: bits 0-3 material, bits 4-5 water energy, bit 6 "queued for sim", bit 7 "moved this tick".
// Bits 4-5 hold a 2-bit "energy" for water (how long it may keep diffusing sideways after being disturbed).
constexpr uint8_t VOX_MASK = 0x0F;
constexpr uint8_t VOX_ENERGY = 0x30;
constexpr int VOX_ENERGY_SHIFT = 4;
constexpr uint8_t VOX_QUEUED = 0x40;
constexpr uint8_t VOX_MOVED = 0x80;

inline uint32_t packCell(int x, int y, int z) { return (uint32_t)x | ((uint32_t)y << 9) | ((uint32_t)z << 18); }
inline void unpackCell(uint32_t c, int& x, int& y, int& z) {
    x = c & 511;
    y = (c >> 9) & 511;
    z = (c >> 18) & 511;
}

struct RayHit {
    bool hit = false;
    float t = 0;
    Vec3 point;
    IVec3 normal;  // face normal of the entered cell
    IVec3 cell;
};

class World {
public:
    static constexpr int CS = 32;
    static constexpr int CVOL = CS * CS * CS;
    static_assert(cfg::WORLD_X % CS == 0 && cfg::WORLD_Y % CS == 0 && cfg::WORLD_Z % CS == 0, "world dims");

    const int SX = cfg::WORLD_X, SY = cfg::WORLD_Y, SZ = cfg::WORLD_Z;
    const int CX = SX / CS, CY = SY / CS, CZ = SZ / CS;

    World();

    // ---- addressing (chunk-contiguous; inside a chunk: x + 32*y + 1024*z, matches a 3D texture) --
    inline int chunkCount() const { return CX * CY * CZ; }
    inline int chunkIndex(int cx, int cy, int cz) const { return cx + CX * (cy + CY * cz); }
    inline bool inBounds(int x, int y, int z) const {
        return (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ;
    }
    inline size_t idx(int x, int y, int z) const {
        size_t ci = (size_t)((x >> 5) + CX * ((y >> 5) + CY * (z >> 5)));
        return (ci << 15) + ((size_t)(z & 31) << 10) + ((size_t)(y & 31) << 5) + (size_t)(x & 31);
    }
    const uint8_t* chunkData(int ci) const { return data.data() + ((size_t)ci << 15); }

    // ---- voxel access --------------------------------------------------------------------------
    inline uint8_t get(int x, int y, int z) const { return inBounds(x, y, z) ? (data[idx(x, y, z)] & VOX_MASK) : (uint8_t)M_AIR; }
    // Material as seen by the mesher: the "ground" below y=0 is solid so floor undersides are culled.
    inline uint8_t getForMesh(int x, int y, int z) const {
        if (y < 0) return ((unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) ? (uint8_t)M_BEDROCK : (uint8_t)M_AIR;
        return get(x, y, z);
    }
    void set(int x, int y, int z, uint8_t m);  // marks dirty + wakes the cellular sim
    inline bool rigidSolid(int x, int y, int z) const {  // rigid bodies: stone + bedrock (+ ground below y=0)
        if (y < 0) return (unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ;
        return isRigidSolid(get(x, y, z));
    }
    inline bool playerSolid(int x, int y, int z) const {  // player: any opaque voxel or body-occupied cell
        if (y < 0) return (unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ;
        if (!inBounds(x, y, z)) return false;
        size_t i = idx(x, y, z);
        return isOpaque(data[i] & VOX_MASK) || overlay[i] != 0;
    }
    inline bool cellFree(int x, int y, int z) const {  // empty and not covered by a rigid body (sim)
        if (!inBounds(x, y, z)) return false;
        size_t i = idx(x, y, z);
        return (data[i] & VOX_MASK) == M_AIR && (!overlayActive || overlay[i] == 0);
    }
    inline bool overlayAt(int x, int y, int z) const { return inBounds(x, y, z) && overlay[idx(x, y, z)] != 0; }

    // ---- change tracking -----------------------------------------------------------------------
    void markDirty(int x, int y, int z);  // chunk meshes (own + neighbours at borders) and texture
    void queueCell(int x, int y, int z);  // add one cellular voxel to the sim's active list (main thread only)
    void wakeAround(int x, int y, int z); // queue the 3x3x3 neighbourhood
    void markAllDirty();

    // Bulk editing: setRaw() changes a voxel without any bookkeeping; call notifyBox() once for the
    // inclusive box that was touched (marks chunks dirty and wakes every cellular voxel in/around it).
    void setRaw(int x, int y, int z, uint8_t m);
    void notifyBox(int x0, int y0, int z0, int x1, int y1, int z1);
    size_t activeCount() const;

    // ---- template / generation -----------------------------------------------------------------
    void generateArena();

    // ---- ray casting (Amanatides & Woo DDA over voxels) ------------------------------------------
    RayHit raycast(const Vec3& origin, const Vec3& dir, float maxDist, uint32_t materialMask) const;

    // ---- cellular automata (CellSim.cpp) ---------------------------------------------------------
    void simTick(Rng& rng);

    // ---- iteration helper: voxel centres within a sphere -----------------------------------------
    template <class F>
    void forEachInSphere(const Vec3& c, float r, F&& f) const {
        int x0 = floorToInt(c.x - r), x1 = floorToInt(c.x + r);
        int y0 = floorToInt(c.y - r), y1 = floorToInt(c.y + r);
        int z0 = floorToInt(c.z - r), z1 = floorToInt(c.z + r);
        float r2 = r * r;
        for (int z = z0; z <= z1; z++)
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    float dx = x + 0.5f - c.x, dy = y + 0.5f - c.y, dz = z + 0.5f - c.z;
                    if (dx * dx + dy * dy + dz * dz <= r2) f(x, y, z);
                }
    }

    // ---- public state ----------------------------------------------------------------------------
    std::vector<uint8_t> data;     // raw voxel bytes
    std::vector<uint8_t> overlay;  // rigid-body occupancy counts (same layout)
    std::vector<uint8_t> meshDirty, texDirty;  // per chunk
    bool overlayActive = false;    // true while any rigid body marks the overlay
    long long counts[5] = {0, 0, 0, 0, 0};
    uint64_t simTickCount = 0;
    int lastTickMoves = 0;

    // ---- cellular sim state: the grid is split into TILE x TILE columns so that tiles of the same
    // checkerboard colour can be simulated on different threads (see CellSim.cpp) ---------------------
    static constexpr int TILE = 8;
    const int TX = SX / TILE, TZ = SZ / TILE;
    inline int tileOf(int x, int z) const { return (x >> 3) + TX * (z >> 3); }
    // Cache-line padding keeps threads working on neighbouring tiles from fighting over the same lines.
    struct alignas(64) TileList {
        std::vector<uint32_t> v;
    };
    struct alignas(64) TileRng {
        Rng r;
    };
    struct alignas(128) SimWorker {
        std::vector<uint32_t> pending;  // cells queued during the tick (any tile)
        std::vector<uint32_t> moved;    // raw indices whose MOVED flag must be cleared
        std::vector<uint32_t> sorted;
        std::vector<int> bucket, cursor;
        int moves = 0;
    };
    std::vector<TileList> tileCur, tileNext;  // active cells per tile
    std::vector<TileRng> tileRng;
    std::vector<SimWorker> simWorkers;

private:
    void simTile(int tile, SimWorker& sw);
};
