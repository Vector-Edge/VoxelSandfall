// Game state + per-frame update (player, tools, rigid bodies, cellular simulation, debris).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "Config.h"
#include "Fire.h"
#include "VoxMath.h"
#include "Physics.h"
#include "Player.h"
#include "Tools.h"
#include "WaterField.h"
#include "World.h"

struct GameInput {
    float forward = 0, strafe = 0;
    bool jump = false, sprint = false;
    float lookDX = 0, lookDY = 0;  // mouse delta in pixels
    bool fireHeld = false;         // left mouse button held
    bool fireClicked = false;      // left mouse button went down this frame
    int wheel = 0;                 // scroll notches (+ = up)
    bool selectCreator = false;    // R
    bool selectGun = false;        // F
    int selectMaterial = 0;        // 1..10 (the 0 key is 10)
    int cycleMaterial = 0;         // -1 / +1
};

struct Particle {
    Vec3 pos, vel;
    float life = 0;
    float size = 1;
    uint8_t mat = M_STONE;
    bool grain = false;  // a displaced sand grain: it flies, then turns back into a sand voxel where it lands
};

enum ToolId { TOOL_CREATOR = 0, TOOL_GUN = 1 };

// A water voxel that a moving body has just pushed out of its way (`surface`: one of the top few of its column).
struct WaterCell {
    int x, y, z;
    bool surface;
};

class Game {
public:
    Game();

    World world;
    Physics physics{world};
    Player player;
    Rng rng{0xC0FFEEu};
    WaterField waterField;  // ripples / foam / smooth surface of the open water (cosmetic; the renderer feeds and draws it)

    int tool = TOOL_CREATOR;
    int materialIndex = 0;  // index into kPaintMaterials
    int brush = cfg::BRUSH_DEFAULT;
    float mouseSens = 0.0022f;

    struct Preview {
        bool valid = false;
        bool onSurface = false;
        Vec3 center;
        float radius = 0;
    } preview;

    // gun presentation
    float recoil = 0, muzzleFlash = 0, gunCooldown = 0;
    int shotsFired = 0;
    CarveResult lastCarve;

    std::vector<Particle> particles;
    float time = 0;
    int lastPlaced = 0;
    int lastSimTicks = 0;

    void reset();
    void update(float dt, const GameInput& in);

    // ---- chunk loading (GameStream.cpp) ---------------------------------------------------------
    // infinite = true: the world is endless (a window of 3 x 3 regions around the player, recentred as the player walks);
    // false: the classic walled arena. Changing it resets the world.
    bool infinite = false;
    void setInfinite(bool on);
    std::vector<std::pair<int, int>> pendingShifts;  // window recentrings (in regions) the renderer has not applied yet
    int regionsShifted = 0;                         // how many recentrings so far (diagnostics)
    // Global voxel position of the player (the window origin plus the local position).
    long long playerGlobalX() const { return world.originX + (long long)std::floor(player.pos.x); }
    long long playerGlobalZ() const { return world.originZ + (long long)std::floor(player.pos.z); }
    size_t stashedBodies() const { return stash_.size(); }

    uint8_t currentMaterial() const { return kPaintMaterials[materialIndex]; }

    // ---- fire, smoke (GameFire.cpp) -------------------------------------------------------------------
    std::vector<FxParticle> fx;         // flames, sparks and smoke
    std::vector<FireLight> fireLights;  // the flames merged into a few glows (rebuilt every frame; the renderer gives them to the shaders)
    void spawnFlames(const Vec3& centre, float radius, int count, float strength = 1.0f);  // the Creator's fire
    void spawnSmokeCloud(const Vec3& centre, float radius, int count, uint8_t kind = FX_SMOKE);
    // Sets the coal / oil at that cell alight (oil: all of it); also a coal rock that has a voxel there. False if nothing can burn.
    bool igniteAt(int x, int y, int z);
    size_t flameCount() const;
    size_t smokeCount() const;

    // Tool actions (also used by the scripted test driver).
    PaintResult paintAtCrosshair();
    CarveResult fireAtCrosshair();

    // ---- sand interaction (SandFx.cpp) -------------------------------------------------------------
    // Footprint under the player's foot (side = -1 left / +1 right): digs a dent and kicks grains backwards.
    void sandFootstep(int side, bool sprint);
    // Impact crater when the player lands on sand at `impactSpeed` (voxels/s): grains spray outwards.
    void sandCrater(float impactSpeed);
    // The player is wading into sand: clears the blocking slab [mn,mx] and flings the grains out to the sides.
    bool sandPlow(const IVec3& mn, const IVec3& mx, int axis, int sign);
    int grainsInFlight() const;
    float lastSplashSpeed = 0;  // speed (voxels/s) of the most recent water impact, for tests / HUD
    char lostInfo[160] = {0};
    int lostGrains = 0;         // displaced voxels that could not be put back anywhere (diagnostics)

private:
    struct StashedBody {
        long long rx = 0, rz = 0;  // global region the body is in
        std::unique_ptr<Body> body;  // pos is relative to the region's corner
    };
    std::vector<StashedBody> stash_;
    void updateStreaming();
    void shiftWorld(int dRx, int dRz);
    void stashBody(std::unique_ptr<Body> b);
    void restoreStashedBodies();
    void updateTools(float dt, const GameInput& in);
    void updateSandInteraction(float dt, const GameInput& in, float velYBefore, bool wasOnGround);
    void updateWaterInteraction(float dt, const Vec3& velBefore);
    void updateBodySplashes();  // rigid bodies pushing into water: bow wave, droplets, ripples
    // Throws displaced water out around `centre`: a bow wave, a crown of droplets and ripples, scaled by how fast the
    // body `vel` is going. `topY` is the highest cell the water was taken from, `rippleScale` scales the surface waves.
    void throwWater(const std::vector<WaterCell>& cells, const Vec3& centre, const Vec3& vel, float halfWidth, int topY, float rippleScale);
    void processSplashes();  // water impacts reported by the cellular sim -> ripples, foam and droplets
    void launchGrain(const Vec3& from, const Vec3& dirOut, float range, bool visible, uint8_t mat = M_SAND, float lift = 0);
    bool placeGrain(int x, int y, int z, uint8_t mat = M_SAND);
    void settleGrain(const Vec3& pos, uint8_t mat = M_SAND);
    int columnTop(int x, int z, int yFrom) const;
    int columnSurface(int x, int z, int yFrom) const;
    template <class F>
    int digSand(float cx, float cz, float radius, float depth, int hintY, F&& onRemoved);
    void spawnDebris(const CarveResult& r);
    void updateParticles(float dt);

    // fire
    void updateFire(float dt);
    void fireStep(float sdt);
    void burnBodies(float sdt);
    void emitFromFires(float dt);
    void updateFx(float dt);
    void updateFireLights();
    void spawnFlame(const Vec3& p, float strength, float calm = 1.0f);  // calm < 1: a flame that lingers (the Creator's fire)
    void spawnSmoke(const Vec3& p, uint8_t kind, float scale);
    bool igniteBodyVoxel(Body& b, int i, int j, int k);
    void flameTouchesBodies(const Vec3& p, float prob);
    World::BurnReport burnRep_;
    float fireAcc_ = 0;
    bool anyBurningBodies_ = false;

    float physAcc_ = 0, simAcc_ = 0, paintTimer_ = 0;
    float stepAccum_ = 0;
    int placeCeiling_ = -1;     // highest surface (in cells) a displaced voxel may be placed on; -1 = default
    float splashScale_ = 1.0f;  // launch-speed multiplier for the event being processed
    int stepSide_ = 1;
    int plowParticleBudget_ = 0;
    std::vector<uint32_t> hollow_;  // cells the player's body has emptied of water and still holds empty (raw indices)
};
