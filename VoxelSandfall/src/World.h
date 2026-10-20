// Voxel world: chunk-contiguous byte grid, cellular-automaton state, ray casting.
#pragma once
#include <unordered_map>
#include <utility>
#include <vector>

#include "Config.h"
#include "VoxMath.h"

enum Material : uint8_t { M_AIR = 0, M_SAND = 1, M_WATER = 2, M_STONE = 3, M_BEDROCK = 4, M_DIRT = 5, M_GRASS = 6 };

// Materials the cellular automaton looks after: sand, water, and dirt / grass (settled dirt is static and only checked
// when something around it changes; see Dirt.cpp).
inline bool isCellular(uint8_t m) { return m == M_SAND || m == M_WATER || m == M_DIRT || m == M_GRASS; }
inline bool isOpaque(uint8_t m) { return m == M_SAND || m >= M_STONE; }   // blocks light / the player
inline bool isRigidSolid(uint8_t m) { return m == M_SAND || m >= M_STONE; }  // what rigid bodies collide with
inline uint32_t matBit(uint8_t m) { return 1u << m; }
// Everything solid a ray can hit in the grid (not water).
constexpr uint32_t kGridSolidMask = (1u << M_SAND) | (1u << M_STONE) | (1u << M_BEDROCK) | (1u << M_DIRT) | (1u << M_GRASS);

// Raw voxel byte: bits 0-3 material, bits 4-5 water energy, bit 6 "queued for sim", bit 7 "moved this tick".
// Bits 4-5 hold a 2-bit "energy" for water: the number of consecutive ticks (capped at 3) it has been free-falling.
// A voxel that lands with energy >= 2 is an impact: it splashes and, on a pool, soaks in (see CellSim.cpp).
constexpr uint8_t VOX_MASK = 0x0F;
constexpr uint8_t VOX_ENERGY = 0x30;
constexpr int VOX_ENERGY_SHIFT = 4;
constexpr uint8_t VOX_QUEUED = 0x40;
constexpr uint8_t VOX_MOVED = 0x80;
// For dirt and grass bits 4-5 are not water energy: bit 4 = loose (falling dirt; cleared when it settles), bit 5 = this
// grass voxel is on the list of grass that may still spread.
constexpr uint8_t VOX_LOOSE = 0x10;
constexpr uint8_t VOX_GACT = 0x20;

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

    // Chunk loading (Stream.cpp): the grid is a window of WIN_REGIONS x WIN_REGIONS regions of REGION x REGION columns.
    // A region is one chunk column; the window slides one column at a time, keeping the player within a column of its centre.
    static constexpr int REGION = 32;                       // streaming unit = 1 x 1 chunk columns
    static constexpr int REGION_CHUNKS = REGION / CS;
    static constexpr int WIN_REGIONS = cfg::WORLD_X / REGION;
    static constexpr int NCOL = (cfg::WORLD_X / CS) * (cfg::WORLD_Z / CS);  // chunk columns in the window
    static_assert(cfg::WORLD_X == REGION * WIN_REGIONS && cfg::WORLD_Z == REGION * WIN_REGIONS, "the window is whole regions");

    const int SX = cfg::WORLD_X, SY = cfg::WORLD_Y, SZ = cfg::WORLD_Z;
    const int CX = SX / CS, CY = SY / CS, CZ = SZ / CS;

    World();

    // ---- addressing (chunk-contiguous; inside a chunk: x + 32*y + 1024*z, matches a 3D texture) --
    inline int chunkCount() const { return CX * CY * CZ; }
    inline int chunkIndex(int cx, int cy, int cz) const { return cx + CX * (cy + CY * cz); }
    inline bool inBounds(int x, int y, int z) const {
        return (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ;
    }
    // Storage is organised in physical blocks of one chunk column (CY chunks); colSlot_ maps the logical chunk column of the
    // window to its block, so the window can scroll without copying anything.
    inline size_t idx(int x, int y, int z) const {
        const size_t ci = (size_t)(colSlot_[(x >> 5) + CX * (z >> 5)] * CY + (y >> 5));
        return (ci << 15) + ((size_t)(z & 31) << 10) + ((size_t)(y & 31) << 5) + (size_t)(x & 31);
    }
    // Voxels of the chunk with the logical index ci (chunkIndex(cx, cy, cz)).
    const uint8_t* chunkData(int ci) const {
        const int cx = ci % CX, cy = (ci / CX) % CY, cz = ci / (CX * CY);
        return data.data() + (((size_t)colSlot_[cx + CX * cz] * CY + (size_t)cy) << 15);
    }
    // Inverse of idx(): the cell a raw index belongs to.
    inline void cellOfIndex(size_t i, int& x, int& y, int& z) const {
        const int pc = (int)(i >> 15), l = (int)(i & 32767);
        const int lc = colLogical_[pc / CY];
        x = (lc % CX) * CS + (l & 31);
        y = (pc % CY) * CS + ((l >> 5) & 31);
        z = (lc / CX) * CS + (l >> 10);
    }

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
        if (boxOn && inPlayerBox(x, y, z)) return false;  // the player is solid to sand and water
        size_t i = idx(x, y, z);
        return (data[i] & VOX_MASK) == M_AIR && (!overlayActive || overlay[i] == 0);
    }
    inline bool inPlayerBox(int x, int y, int z) const {
        return x >= boxMin[0] && x <= boxMax[0] && y >= boxMin[1] && y <= boxMax[1] && z >= boxMin[2] && z <= boxMax[2];
    }
    // The player's cell box (inclusive). While set, sand and water neither enter nor settle inside it.
    void setPlayerBox(int x0, int y0, int z0, int x1, int y1, int z1) {
        boxOn = true;
        boxMin[0] = x0; boxMin[1] = y0; boxMin[2] = z0;
        boxMax[0] = x1; boxMax[1] = y1; boxMax[2] = z1;
    }
    void clearPlayerBox() { boxOn = false; }
    inline bool overlayAt(int x, int y, int z) const { return inBounds(x, y, z) && overlay[idx(x, y, z)] != 0; }

    // Coarse solidity map for the rigid body solver: true when any 8x8x8 brick overlapping the inclusive cell box holds
    // sand, stone or bedrock (below y = 0 is the solid ground). Bricks are summarised lazily, per chunk, after edits.
    bool anySolidIn(int x0, int y0, int z0, int x1, int y1, int z1) const;

    // ---- change tracking -----------------------------------------------------------------------
    void markDirty(int x, int y, int z);  // chunk meshes (own + neighbours at borders) and texture
    // Same, for a change that only swaps water and air: only the water meshes need rebuilding (the opaque mesh, the
    // sun shadow map and the opaque greedy pass are untouched).
    void markDirtyWater(int x, int y, int z);
    void queueCell(int x, int y, int z);  // add one cellular voxel to the sim's active list (main thread only)
    void wakeAround(int x, int y, int z); // queue the 3x3x3 neighbourhood
    void markAllDirty();

    // Bulk editing: setRaw() changes a voxel without any bookkeeping; call notifyBox() once for the
    // inclusive box that was touched (marks chunks dirty and wakes every cellular voxel in/around it).
    void setRaw(int x, int y, int z, uint8_t m);
    void notifyBox(int x0, int y0, int z0, int x1, int y1, int z1, bool waterOnly = false);
    // After water was removed from the inside of a box: refresh the water meshes there and wake only the water touching
    // the box from outside (instead of scanning every voxel inside it).
    void notifyBoxWater(int x0, int y0, int z0, int x1, int y1, int z1);
    // Puts one water voxel into an air cell (a displaced / splashed voxel landing). Unlike set() it does not wake the
    // whole neighbourhood: the voxel below and, now and then, the voxel itself (a surface voxel's level scan is costly).
    void setWaterQuiet(int x, int y, int z);
    size_t activeCount() const;

    // ---- template / generation -----------------------------------------------------------------
    void generateArena();  // the classic walled 256 x 256 arena (fixed world, no chunk loading)

    // ---- chunk loading (Stream.cpp) --------------------------------------------------------------
    uint32_t epoch = 0;                     // bumped whenever the whole world is replaced (reset / mode change)
    bool streaming = false;                 // true: the grid is a scrolling window of an infinite world
    long long originX = 0, originZ = 0;     // global voxel coordinate of local (0, 0); multiples of REGION
    static constexpr int CENTER = WIN_REGIONS / 2;  // index of the window's middle region (where the player stands)
    std::unordered_map<uint64_t, std::vector<uint8_t>> savedRegions;  // modified regions that left the window (compressed)
    std::vector<uint8_t> chunkModified;     // per logical chunk: changed since it was loaded (saved when it leaves)
    std::vector<uint8_t> chunkUrgent;       // per logical chunk: freshly loaded - the renderer meshes it before drawing
    struct ShiftResult {
        std::vector<std::pair<long long, long long>> loaded, evicted;  // global region coordinates
        std::vector<std::pair<long long, long long>> restored;         // (the part of `loaded` that came from the store, not generated)
    };
    void initStreaming();                   // empty infinite world: a flat floor, the window centred on region (0, 0)
    // Recentres the window by whole regions (the player walked into a neighbouring one). Does not touch anything that
    // holds positions outside the grid (player, bodies, particles, GPU data): the owner shifts those by -dRx*REGION etc.
    ShiftResult shiftWindow(int dRx, int dRz);
    bool regionLoaded(long long rx, long long rz) const;
    size_t savedBytes() const;
    long long globalX(float localX) const { return originX + (long long)std::floor(localX); }
    long long globalZ(float localZ) const { return originZ + (long long)std::floor(localZ); }

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
    std::vector<uint8_t> waterDirty;           // per chunk: only the water mesh is stale
    mutable std::vector<uint8_t> brickSolid, summaryDirty;  // 64 brick flags per chunk / chunk needs re-summarising
    bool overlayActive = false;    // true while any rigid body marks the overlay
    bool boxOn = false;
    int boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};
    long long counts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t quietCounter = 0;
    uint64_t simTickCount = 0;
    int lastTickMoves = 0;

    // Water impacts: a voxel that has been falling lands. The owner (Game) drains this list every frame and turns the
    // events into ripples, foam and flying droplets. The list is capped, so nothing needs to drain it in tests.
    struct SplashEvent {
        float x, y, z;     // where the water hit (cell centre)
        uint8_t power;     // 2..3: how long the voxel had been falling
        uint8_t droplet;   // 1 = the voxel was taken out of the grid and the owner must put it back (as a flying droplet)
    };
    std::vector<SplashEvent> splashes;
    int dropletBudget = 0;  // droplets the owner will accept in the next tick; 0 = never turn voxels into droplets
    double dbgImpactMs = 0, dbgHopMs = 0, dbgParMs = 0;  // accumulated serial/parallel phase times (diagnostics)
    long long dbgImpacts = 0, dbgVisits = 0, dbgMoves = 0;
    bool soakEnabled = true;
    bool impactsEnabled = true;  // false: landed water just stops (no splash events, no soaking) - for comparisons

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
        std::vector<uint64_t> hops;  // water surface hops proposed this tick: (src << 32) | dst
        std::vector<uint32_t> impacts;  // landed water voxels: packCell | (energy << 27)
        std::vector<uint32_t> dirtSeeds, grassSeeds;  // static dirt that may be hanging / grass that may spread (packed cells)
        int moves = 0;
    };
    // ---- dirt & grass (Dirt.cpp) ---------------------------------------------------------------------
    // A hanging mass of dirt holds while  mass <= kDirtStrength * (faces that tie it to the supported structure).
    static constexpr int kDirtStrength = 14;
    std::vector<uint32_t> dirtQueue;     // static dirt voxels to check for structural failure (packed cells)
    std::vector<uint32_t> grassActive;   // grass voxels that may still spread (flagged VOX_GACT)
    std::vector<uint8_t> dirtMemo;       // scratch flags for the structure analysis (cleared after every run)
    size_t grassCursor = 0, dirtHead = 0;   // dirtHead: first unprocessed entry of dirtQueue
    uint32_t dirtCollapsed = 0;          // voxels let go by structural failures so far (diagnostics / tests)
    Rng grassRng{0x6A55u};
    void setLooseDirt(int x, int y, int z);  // a dirt voxel that falls and settles
    bool paintGrass(int x, int y, int z);    // settled dirt -> grass, if it is in the surface skin; false if not legal
    int grassDepth(int x, int y, int z) const;
    void analyzeDirt(int maxSeeds);
    void growGrass(int maxEntries);

    std::vector<TileList> tileCur, tileNext;  // active cells per tile
    std::vector<TileRng> tileRng;
    std::vector<SimWorker> simWorkers;

private:
    int colSlot_[NCOL];     // logical chunk column -> physical block
    int colLogical_[NCOL];  // and back
    void resetColumns();
    uint8_t* physChunk(int physCol, int cy);
    void regionCounts(int rxl, int rzl, long long sign);
    void generatedCounts(long long sign);  // what one generated region adds to (sign > 0) / removes from the counts
    void generateRegionInto(int rxl, int rzl);
    void saveRegionTo(int rxl, int rzl, std::vector<uint8_t>& out);
    void loadRegionFrom(int rxl, int rzl, const std::vector<uint8_t>& in);
    void wakeRegion(int rxl, int rzl);
    void wakeSeam(int rxl, int rzl, int side);
    bool regionIsNew(const ShiftResult& r, int rxl, int rzl) const;
    void markDirtyImpl(int x, int y, int z, std::vector<uint8_t>& meshFlags);
    void simTile(int tile, SimWorker& sw);
    int applyWaterHops(int maxHops);
    int applyImpacts(int maxImpacts);
    void wakeAfterMove(int sx, int sy, int sz);
    Rng impactRng{0xA17F3u};
};
