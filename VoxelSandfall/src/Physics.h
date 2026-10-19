// Custom rigid body physics for Normal Stone bodies (spec sections 1 and 2).
// A body is a lattice of unit voxels. Collision shapes are one sphere per surface voxel; contacts are
// resolved with a warm-started sequential-impulse solver, friction and sleeping.
#pragma once
#include <memory>
#include <vector>

#include "VoxMath.h"
#include "World.h"

struct Body {
    int id = 0;
    int nx = 0, ny = 0, nz = 0;      // lattice dimensions
    std::vector<uint8_t> occ;        // nx*ny*nz occupancy
    int count = 0;
    Vec3 com;                        // centre of mass in lattice space
    Vec3 pos;                        // world position of the COM
    Quat rot;
    Vec3 vel, ang;
    float mass = 1, invMass = 1;
    Mat3 invIbody = Mat3::identity(), invIworld = Mat3::identity();
    std::vector<Vec3> surf;          // surface voxel centres relative to the COM (body frame)
    std::vector<Vec3> warm, warmPrev;  // per surface voxel warm-start impulses (normal, tangent1, tangent2)
    float radius = 1;                // bounding sphere radius about the COM
    bool asleep = false;
    bool dead = false;
    bool touching = false;           // had a contact this step (rolling resistance)
    float sleepTimer = 0;
    uint32_t version = 1;            // bumped whenever the voxel set changes (mesh rebuild)
    std::vector<uint32_t> ovCells;   // world overlay cells currently marked by this body
    bool ovDirty = true;

    bool occupiedAt(int i, int j, int k) const {
        return (unsigned)i < (unsigned)nx && (unsigned)j < (unsigned)ny && (unsigned)k < (unsigned)nz &&
               occ[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)] != 0;
    }
    Vec3 toWorld(const Vec3& bodyFrame) const { return pos + rot.rotate(bodyFrame); }
    Vec3 latticeToWorld(const Vec3& lat) const { return toWorld(lat - com); }
    Vec3 worldToLattice(const Vec3& w) const { return rot.conjugate().rotate(w - pos) + com; }

    // Replace the voxel set. `cells` are integer lattice coordinates in the *current* lattice
    // (they may lie outside it). The world placement of every voxel is preserved.
    void assignCells(const std::vector<IVec3>& cells);
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
    Body* createFromWorldCells(const std::vector<IVec3>& cells);
    // Replace a body's voxels (lattice coords). Empty -> removed, disconnected -> split into several.
    void setBodyCells(Body& b, const std::vector<IVec3>& cells);

    void step(float dt);
    void wakeNear(const Vec3& c, float r);
    void wakeAll();
    void rasterizeOverlay();  // keeps World::overlay in sync with the bodies (sand/water/player see them)
    void purgeDead();
    size_t awakeCount() const;

private:
    void collideWorld(Body& a, std::vector<Contact>& out);
    void collidePair(Body& a, Body& b, std::vector<Contact>& out);
    void solve(std::vector<Contact>& cs, float dt);
    void clearOverlay(Body& b);

    std::vector<Contact> contacts_;
    std::vector<Body*> sortedBodies_;
};
