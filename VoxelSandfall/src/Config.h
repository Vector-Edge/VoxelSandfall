// Global tunables for the Version 0.0.1 prototype. One voxel == one world unit.
#pragma once

namespace cfg {

// --- Template arena (spec section 5) ---------------------------------------------------------
constexpr int ARENA_SIZE = 256;   // bedrock floor is ARENA_SIZE x ARENA_SIZE x 1
constexpr int WALL_THICK = 3;     // perimeter walls of Normal Stone, 3 voxels thick
constexpr int WALL_HEIGHT = 160;  // wall height above the floor layer (voxels)

// World grid (multiples of the 32-voxel chunk size). In the endless world this is the loaded window around the player
// (15 x 15 chunk columns); the classic arena sits in its corner.
constexpr int WORLD_X = 480;
constexpr int WORLD_Y = 256;
constexpr int WORLD_Z = 480;

constexpr int ARENA_TOTAL = ARENA_SIZE + 2 * WALL_THICK;  // floor + walls on each side
constexpr int ARENA_OX = 13;                               // wall outer corner
constexpr int ARENA_OZ = 13;
constexpr int FLOOR_X0 = ARENA_OX + WALL_THICK;            // floor extent [X0, X1)
constexpr int FLOOR_X1 = FLOOR_X0 + ARENA_SIZE;
constexpr int FLOOR_Z0 = ARENA_OZ + WALL_THICK;
constexpr int FLOOR_Z1 = FLOOR_Z0 + ARENA_SIZE;

// --- Player (spec section 3): 50 x 100 x 50 voxel bounding box ------------------------------
constexpr float PLAYER_W = 50.0f;
constexpr float PLAYER_H = 100.0f;
constexpr float PLAYER_EYE = 90.0f;  // eye height near the top of the 100-voxel box
constexpr float PLAYER_CORE = 22.0f; // width of the torso that displaces water / blocks sand (inside the 50-wide box)
constexpr float WALK_SPEED = 170.0f;
constexpr float SPRINT_SPEED = 300.0f;
constexpr float JUMP_SPEED = 290.0f;
constexpr float GRAVITY = 700.0f;       // voxels / s^2 (1 voxel ~ 1.8 cm)
constexpr float STEP_HEIGHT = 6.0f;     // auto step-up so the player can climb sand/rubble
constexpr float PLOW_HEIGHT = 20.0f;    // the player wades through (and shoves aside) loose sand up to this high

// --- Simulation ------------------------------------------------------------------------------
constexpr float CELL_TICK_HZ = 120.0f;  // falling-sand / fluid automaton rate (voxels fall up to 3 cells per tick)
constexpr int   CELL_MAX_TICKS = 6;     // per frame
constexpr float SIM_BUDGET_MS = 7.0f;   // stop ticking once a frame has spent this long (sim slows down, frame does not)
constexpr float PHYS_HZ = 240.0f;       // rigid body fixed step
constexpr int   PHYS_MAX_STEPS = 6;
constexpr float PHYS_BUDGET_MS = 4.0f;  // rigid bodies get at most this long per frame (beyond it they run in slow motion)

// --- Tools ------------------------------------------------------------------------------------
constexpr int   MAX_PARTICLES = 6000;  // flying grains / droplets / debris
constexpr int   BRUSH_MIN = 1;
constexpr int   BRUSH_MAX = 48;
constexpr int   BRUSH_DEFAULT = 4;
constexpr float REACH = 600.0f;            // creator / gun ray length
constexpr float FREE_PAINT_DIST = 130.0f;  // paint distance when the ray hits nothing
constexpr float GUN_RPM = 650.0f;

}  // namespace cfg
