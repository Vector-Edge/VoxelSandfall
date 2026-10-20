#include "Player.h"

namespace {
constexpr float kEps = 0.005f;
constexpr float kSkin = 0.001f;

// Does the slab of cells at index `k` along `a`, spanning [b0,b1] x [c0,c1], contain a solid?
bool slabSolid(const World& w, int a, int k, int b, int b0, int b1, int c, int c0, int c1) {
    int cell[3];
    cell[a] = k;
    for (int bb = b0; bb <= b1; bb++) {
        cell[b] = bb;
        for (int cc = c0; cc <= c1; cc++) {
            cell[c] = cc;
            if (w.playerSolid(cell[0], cell[1], cell[2])) return true;
        }
    }
    return false;
}
}  // namespace

// Coarse sampled test (5 x 5 columns, every 6 voxels in height) used to detect sand or a rigid body
// landing inside the player box; the exact sweeps below assume the box starts out free.
bool Player::overlapsSolid(const World& w) const {
    const float hw = cfg::PLAYER_W * 0.5f - 0.05f;
    for (int iz = 0; iz < 5; iz++)
        for (int ix = 0; ix < 5; ix++) {
            const float x = pos.x - hw + 2.0f * hw * ix / 4.0f, z = pos.z - hw + 2.0f * hw * iz / 4.0f;
            for (float y = pos.y + 0.3f; y < pos.y + cfg::PLAYER_H - 0.1f; y += 6.0f)
                if (w.playerSolid(floorToInt(x), floorToInt(y), floorToInt(z))) return true;
        }
    return false;
}

// Fraction of the body (8 heights between the feet and the head) that has water around it. The player's own
// box is kept free of water by the game, so the cells just outside the box are sampled as well as the centre.
float Player::sampleWater(const World& w) const {
    const float hw = cfg::PLAYER_CORE * 0.5f + 2.5f;
    int wet = 0;
    for (int i = 0; i < 8; i++) {
        const int y = floorToInt(pos.y + (i + 0.5f) * cfg::PLAYER_H / 8.0f);
        if (w.get(floorToInt(pos.x), y, floorToInt(pos.z)) == M_WATER || w.get(floorToInt(pos.x + hw), y, floorToInt(pos.z)) == M_WATER ||
            w.get(floorToInt(pos.x - hw), y, floorToInt(pos.z)) == M_WATER || w.get(floorToInt(pos.x), y, floorToInt(pos.z + hw)) == M_WATER ||
            w.get(floorToInt(pos.x), y, floorToInt(pos.z - hw)) == M_WATER)
            wet++;
    }
    return (float)wet / 8.0f;
}

void Player::respawn() {
    pos = Vec3(cfg::ARENA_OX + cfg::ARENA_TOTAL * 0.5f, 1.0f + kSkin, cfg::ARENA_OZ + cfg::ARENA_TOTAL * 0.5f + 60.0f);
    vel = Vec3(0, 0, 0);
    yaw = 0;
    pitch = 0;
    onGround = false;
}

void Player::respawnAt(const Vec3& feet) {
    pos = Vec3(feet.x, feet.y + kSkin, feet.z);
    vel = Vec3(0, 0, 0);
    yaw = 0;
    pitch = 0;
    onGround = false;
}

// Sweep the AABB along one axis, stopping flush against the first solid voxel slab. Returns the
// displacement actually applied.
float Player::sweepAxis(const World& w, int a, float d) {
    if (d == 0) return 0;
    const float hw = cfg::PLAYER_W * 0.5f;
    const float mn[3] = {pos.x - hw, pos.y, pos.z - hw};
    const float mx[3] = {pos.x + hw, pos.y + cfg::PLAYER_H, pos.z + hw};
    const int b = (a + 1) % 3, c = (a + 2) % 3;
    const int b0 = floorToInt(mn[b] + kEps), b1 = (int)std::ceil(mx[b] - kEps) - 1;
    const int c0 = floorToInt(mn[c] + kEps), c1 = (int)std::ceil(mx[c] - kEps) - 1;
    float moved = d;
    auto tryPlow = [&](int ax, int k, int bb, int bb0, int bb1, int cc, int cc0, int cc1, int sign) {
        if (!plowEnabled_ || !plowHook || ax == 1) return false;
        IVec3 lo, hi;
        lo[ax] = hi[ax] = k;
        lo[bb] = bb0; hi[bb] = bb1;
        lo[cc] = cc0; hi[cc] = cc1;
        return plowHook(lo, hi, ax, sign);
    };
    if (d > 0) {
        const float M = mx[a];
        const int first = floorToInt(M - kEps) + 1, last = (int)std::ceil(M + d) - 1;
        for (int k = first; k <= last; k++)
            if (slabSolid(w, a, k, b, b0, b1, c, c0, c1)) {
                if (tryPlow(a, k, b, b0, b1, c, c0, c1, 1)) continue;
                moved = std::max(0.0f, (float)k - M - kSkin);
                break;
            }
    } else {
        const float m = mn[a];
        const int first = floorToInt(m + kEps) - 1, last = floorToInt(m + d);
        for (int k = first; k >= last; k--)
            if (slabSolid(w, a, k, b, b0, b1, c, c0, c1)) {
                if (tryPlow(a, k, b, b0, b1, c, c0, c1, -1)) continue;
                moved = -std::max(0.0f, m - (float)(k + 1) - kSkin);
                break;
            }
    }
    pos[a] += moved;
    return moved;
}

void Player::moveHorizontal(const World& w, int axis, float d) {
    if (d == 0) return;
    const Vec3 start = pos;
    plowEnabled_ = false;
    const float moved = sweepAxis(w, axis, d);
    const bool blocked = std::fabs(moved) < std::fabs(d) - 1e-3f;
    if (!blocked) return;
    const Vec3 afterNormal = pos;
    if (onGround) {  // first try stepping up onto a low ledge (sand slopes, rubble, pebbles)
        pos = start;
        const float up = sweepAxis(w, 1, cfg::STEP_HEIGHT);
        const float moved2 = sweepAxis(w, axis, d);
        const float down = sweepAxis(w, 1, -(up + 0.5f));
        const bool landed = down > -(up + 0.5f) + 1e-3f;
        if (landed && std::fabs(moved2) > std::fabs(moved) + 0.05f) return;
        pos = afterNormal;
    }
    // Too high to climb. A rock in the way is shoved (the heavier, the slower)...
    if (pushHook) {
        const float hw = cfg::PLAYER_W * 0.5f;
        const int sgn = d > 0 ? 1 : -1;
        const float face = sgn > 0 ? pos[axis] + hw : pos[axis] - hw;
        const int k = sgn > 0 ? floorToInt(face) + 1 : floorToInt(face) - 1;
        const int b = (axis + 1) % 3, c = (axis + 2) % 3;
        const float mn[3] = {pos.x - hw, pos.y, pos.z - hw}, mx[3] = {pos.x + hw, pos.y + cfg::PLAYER_H, pos.z + hw};
        IVec3 lo, hi;
        lo[axis] = hi[axis] = k;
        lo[b] = floorToInt(mn[b] + kEps);
        hi[b] = (int)std::ceil(mx[b] - kEps) - 1;
        lo[c] = floorToInt(mn[c] + kEps);
        hi[c] = (int)std::ceil(mx[c] - kEps) - 1;
        const float cap = pushHook(lo, hi, axis, sgn, std::fabs(vel[axis]));
        if (cap >= 0) {
            vel[axis] = sgn * std::min(std::fabs(vel[axis]), cap);
            return;
        }
    }
    if (onGround) {
        // ...and loose sand is shoved aside.
        if (plowHook && !inWater) {
            pos = start;
            plowEnabled_ = true;
            const float moved3 = sweepAxis(w, axis, d);
            plowEnabled_ = false;
            if (std::fabs(moved3) > std::fabs(moved) + 1e-3f) {
                if (std::fabs(moved3) < std::fabs(d) - 1e-3f) vel[axis] *= 0.6f;  // sand drags at the legs
                return;
            }
            pos = afterNormal;
        }
    }
    vel[axis] = 0;
}

void Player::update(World& w, float dt, const PlayerInput& in) {
    if (frozen) return;
    if (overlapsSolid(w)) {  // buried by falling sand / a body: float up until free
        pos.y += 180.0f * dt;
        vel.y = std::max(vel.y, 0.0f);
        return;
    }
    const Vec3 fwd(-std::sin(yaw), 0, -std::cos(yaw));
    const Vec3 right(std::cos(yaw), 0, -std::sin(yaw));
    Vec3 wish = fwd * in.forward + right * in.strafe;
    if (lengthSq(wish) > 1.0f) wish = normalize(wish);

    waterFrac = sampleWater(w);
    inWater = waterFrac > 0.12f;
    float speed = in.sprint ? cfg::SPRINT_SPEED : cfg::WALK_SPEED;
    speed *= 1.0f - 0.55f * waterFrac;  // wading drags at the legs
    wish = wish * speed;

    const float accel = onGround ? (inWater ? 7.0f : 12.0f) : (inWater ? 5.0f : 2.5f);
    const float t = 1.0f - std::exp(-accel * dt);
    vel.x += (wish.x - vel.x) * t;
    vel.z += (wish.z - vel.z) * t;

    if (inWater) {
        // Buoyancy: neutral when ~65% of the body is submerged; drag grows with submersion.
        vel.y -= cfg::GRAVITY * (1.0f - 1.55f * waterFrac) * dt;
        vel.y *= 1.0f / (1.0f + (1.5f + 5.0f * waterFrac) * dt);
        vel.x *= 1.0f / (1.0f + 1.2f * waterFrac * dt);
        vel.z *= 1.0f / (1.0f + 1.2f * waterFrac * dt);
        if (in.jump) {
            if (waterFrac > 0.55f) {
                vel.y += 1500.0f * dt;  // kick upwards to swim / break the surface
            } else if (onGround) {
                vel.y = cfg::JUMP_SPEED * (1.0f - 0.35f * waterFrac);
                onGround = false;
            }
        }
    } else {
        if (in.jump && onGround) {
            vel.y = cfg::JUMP_SPEED;
            onGround = false;
        }
        vel.y -= cfg::GRAVITY * dt;
    }
    vel.y = std::max(vel.y, -900.0f);

    moveHorizontal(w, 0, vel.x * dt);
    moveHorizontal(w, 2, vel.z * dt);

    const float dy = vel.y * dt;
    const float moved = sweepAxis(w, 1, dy);
    if (std::fabs(moved) < std::fabs(dy) - 1e-4f) {
        if (dy < 0) onGround = true;
        vel.y = 0;
    } else {
        onGround = false;
    }

    if (pos.y < -400.0f) respawn();
}
