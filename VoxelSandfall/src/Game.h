// Game state + per-frame update (player, tools, rigid bodies, cellular simulation, debris).
#pragma once
#include <string>
#include <vector>

#include "Config.h"
#include "VoxMath.h"
#include "Physics.h"
#include "Player.h"
#include "Tools.h"
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
    int selectMaterial = 0;        // 1..4
    int cycleMaterial = 0;         // -1 / +1
};

struct Particle {
    Vec3 pos, vel;
    float life = 0;
    float size = 1;
    uint8_t mat = M_STONE;
};

enum ToolId { TOOL_CREATOR = 0, TOOL_GUN = 1 };

class Game {
public:
    Game();

    World world;
    Physics physics{world};
    Player player;
    Rng rng{0xC0FFEEu};

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

private:
    void updateTools(float dt, const GameInput& in);
    void spawnDebris(const CarveResult& r);
    void updateParticles(float dt);

    float physAcc_ = 0, simAcc_ = 0, paintTimer_ = 0;
};
