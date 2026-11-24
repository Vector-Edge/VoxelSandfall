// Custom rigid body physics for Normal Stone bodies (spec sections 1 and 2).
// A body is a lattice of unit voxels. Collision shapes are one sphere per surface voxel; contacts are
// resolved with a warm-started sequential-impulse solver, friction and sleeping.
#pragma once
#include <memory>
#include <vector>

#include "VoxMath.h"
#include "World.h"

// A burning coal voxel of a body (lattice coordinates) and the seconds it has left.
struct BodyBurn {
    int i, j, k;
    float left;
};

struct Body {
    int id = 0;
    int nx = 0, ny = 0, nz = 0;      // lattice dimensions
    std::vector<uint8_t> occ;        // nx*ny*nz: 0 = empty, otherwise the material of the voxel (stone, coal, burning coal)
    std::vector<BodyBurn> burn;      // the burning voxels (their occ value is M_BCOAL)
    bool burnDirty = false;          // the burning state changed: the mesh is stale (refreshed a few times a second)
    float burnMeshT = 0, burnCutT = 0;  // seconds since the mesh was refreshed / since burnt-away voxels were cut out
    int count = 0;
    Vec3 com;                        // centre of mass in lattice space
    Vec3 pos;                        // world position of the COM
    Quat rot;
    Vec3 vel, ang;
    float mass = 1, invMass = 1;
    Mat3 invIbody = Mat3::identity(), invIworld = Mat3::identity();
    std::vector<Vec3> surf;          // surface voxel centres relative to the COM (body frame)
    std::vector<Vec3> colPts;        // where those collide with the world: the same points, or (round rocks) pulled onto a true sphere
    bool round = false;              // roughly a ball: it collides as one, so it rolls instead of tipping over the facets of its voxels
    std::vector<Vec3> warm, warmPrev;  // per surface voxel warm-start impulses (normal, tangent1, tangent2)
    float radius = 1;                // bounding sphere radius about the COM
    bool asleep = false;
    bool dead = false;
    bool touching = false;           // had a contact this step (rolling resistance)
    float sleepTimer = 0;
    uint32_t version = 1;            // bumped whenever the voxel set changes (mesh rebuild)
    std::vector<uint32_t> ovCells;   // world overlay cells currently marked by this body
    bool ovDirty = true;
    Vec3 ovPos;                      // pose the overlay was last rasterised at
    Quat ovRot;
    bool ovValid = false;
    Vec3 sandF, sandT;               // force / torque the sand heaped against the body pushes it with (re-measured now and then)
    float sandTimer = 0;

    bool occupiedAt(int i, int j, int k) const {
        return (unsigned)i < (unsigned)nx && (unsigned)j < (unsigned)ny && (unsigned)k < (unsigned)nz &&
               occ[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)] != 0;
    }
    uint8_t matAt(int i, int j, int k) const {  // 0 outside / empty
        return ((unsigned)i < (unsigned)nx && (unsigned)j < (unsigned)ny && (unsigned)k < (unsigned)nz)
                   ? occ[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)]
                   : (uint8_t)0;
    }
    uint8_t& matRef(int i, int j, int k) { return occ[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)]; }
    Vec3 toWorld(const Vec3& bodyFrame) const { return pos + rot.rotate(bodyFrame); }
    Vec3 latticeToWorld(const Vec3& lat) const { return toWorld(lat - com); }
    Vec3 worldToLattice(const Vec3& w) const { return rot.conjugate().rotate(w - pos) + com; }

    // Replace the voxel set. `cells` are integer lattice coordinates in the *current* lattice
    // (they may lie outside it). The world placement of every voxel is preserved.
    // `mats` (optional, parallel to `cells`) gives each voxel's material; without it a voxel keeps the material it has in the
    // current lattice, and one that is not in it yet gets `newMat`.
    void assignCells(const std::vector<IVec3>& cells, uint8_t newMat = M_STONE, const std::vector<uint8_t>* mats = nullptr);
    void collectCells(std::vector<IVec3>& out) const;
    void computeMassProps();
    void updateInertiaWorld();
    void wake() { asleep = false; sleepTimer = 0; ovDirty = true; }
};

struct Contact {
    Body* a = nullptr;   // dynamic body
    Body* b = nullptr;   // other body, or nullptr for the static world
    Vec3 n;              // normal pointing from b / world towards a
    Vec3 ra, rb;         // contact arms from the centres of mass
    float depth = 0;
    int si = -1;         // surface voxel index (world contacts, warm starting)
    Vec3 t1, t2;
    float mn = 0, mt1 = 0, mt2 = 0;
    float bias = 0;
    float jn = 0, jt1 = 0, jt2 = 0;
};

class Physics {
public:
    explicit Physics(World& w) : world(w) {}

    World& world;
    std::vector<std::unique_ptr<Body>> bodies;
    int nextId = 1;
    int lastContactCount = 0;

    // Create a dynamic body from world-space voxel cells (axis aligned, identity transform).
    Body* createFromWorldCells(const std::vector<IVec3>& cells, const std::vector<uint8_t>* mats = nullptr, uint8_t mat = M_STONE);
    // Replace a body's voxels (lattice coords). Empty -> removed, disconnected -> split into several. Voxels that are not in
    // the lattice yet get `newMat`.
    void setBodyCells(Body& b, const std::vector<IVec3>& cells, uint8_t newMat = M_STONE);
    // The face-connected pieces of a set of cells.
    static void connectedComponents(const std::vector<IVec3>& cells, std::vector<std::vector<IVec3>>& out);

    void step(float dt);
    void wakeNear(const Vec3& c, float r);
    void wakeAll();
    void clearOverlays();     // every body's overlay marks are removed (before the world's storage is remapped)
    void rasterizeOverlay();  // keeps World::overlay in sync with the bodies (sand/water/player see them)
    void purgeDead();
    size_t awakeCount() const;
    // The player (moving at `speed` along `axis` towards `sign`) is blocked by the slab of cells [lo, hi]: shoves the bodies in
    // it (heavy ones barely move). Returns the speed the player can keep going at, or -1 when no body is in the slab.
    float pushFromPlayer(const IVec3& lo, const IVec3& hi, int axis, int sign, float speed);

private:
    void collideWorld(Body& a, std::vector<Contact>& out);
    void collidePair(Body& a, Body& b, std::vector<Contact>& out);
    void solve(std::vector<Contact>& cs, float dt);
    void clearOverlay(Body& b);
    void sandPressure(float dt);
    void evalSandPressure(Body& b);
    std::vector<uint64_t> sandKeys_;
    std::vector<uint32_t> oldCells_;

    std::vector<Contact> contacts_;
    std::vector<Body*> sortedBodies_;
};
