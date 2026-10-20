// First-person controller: a 50 x 100 x 50 voxel AABB centred on the camera column (spec section 3).
#pragma once
#include <functional>

#include "VoxMath.h"
#include "World.h"

struct PlayerInput {
    float forward = 0;  // -1..1
    float strafe = 0;   // -1..1 (right positive)
    bool jump = false;
    bool sprint = false;
};

class Player {
public:
    Vec3 pos;  // centre of the feet (bottom of the box)
    Vec3 vel;
    float yaw = 0, pitch = 0;
    bool onGround = false;
    bool inWater = false;
    float waterFrac = 0;  // 0..1: how much of the body is below the water surface
    bool frozen = false;  // scripted camera shots: no gravity / movement
    // Called when a horizontal move is blocked by a slab of voxels [mn, mx] (inclusive cell box); returns true if
    // the slab was cleared (sand shoved aside) so the player may keep going.
    std::function<bool(const IVec3& mn, const IVec3& mx, int axis, int sign)> plowHook;

    void respawn();
    void update(World& w, float dt, const PlayerInput& in);

    Vec3 eye() const { return pos + Vec3(0, cfg::PLAYER_EYE, 0); }
    Vec3 forwardDir() const {
        float cp = std::cos(pitch);
        return {-std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
    }
    Vec3 rightDir() const { return {std::cos(yaw), 0, -std::sin(yaw)}; }
    void aabb(Vec3& mn, Vec3& mx) const {
        const float hw = cfg::PLAYER_W * 0.5f;
        mn = Vec3(pos.x - hw, pos.y, pos.z - hw);
        mx = Vec3(pos.x + hw, pos.y + cfg::PLAYER_H, pos.z + hw);
    }

private:
    bool overlapsSolid(const World& w) const;
    float sampleWater(const World& w) const;
    float sweepAxis(const World& w, int axis, float d);
    bool plowEnabled_ = false;
    void moveHorizontal(const World& w, int axis, float d);
};
