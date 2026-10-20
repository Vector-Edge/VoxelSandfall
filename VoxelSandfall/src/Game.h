// Game state + per-frame update (player, tools, rigid bodies, cellular simulation, debris).
#pragma once
#include <string>
#include <vector>

#include "Config.h"
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
    int selectMaterial = 0;        // 1..6
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
    uint8_t currentMaterial() const { return kPaintMaterials[materialIndex]; }

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

    float physAcc_ = 0, simAcc_ = 0, paintTimer_ = 0;
    float stepAccum_ = 0;
    int placeCeiling_ = -1;     // highest surface (in cells) a displaced voxel may be placed on; -1 = default
    float splashScale_ = 1.0f;  // launch-speed multiplier for the event being processed
    int stepSide_ = 1;
    int plowParticleBudget_ = 0;
    std::vector<uint32_t> hollow_;  // cells the player's body has emptied of water and still holds empty (raw indices)
};
