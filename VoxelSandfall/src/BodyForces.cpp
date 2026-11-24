// Forces on rigid bodies that do not come from contacts: the player pushing a rock, and the pressure of a heap of sand.
#include <algorithm>

#include "Physics.h"

namespace {
// The player as a pushing mass, in units of "voxels of stone" (a 1 voxel ~ 1.8 cm cube of rock weighs ~15 g, a person ~80 kg):
// a football-sized rock (~900 voxels) hardly slows the player, a 1 m boulder (~100 000) barely creeps.
constexpr float kPlayerMass = 5300.0f;
constexpr float kCreepSpeed = 18.0f;  // however heavy, a body pushed on keeps creeping along at about this (voxels / s)

// Sand against a body: lateral earth pressure = K * (weight of the sand column above the contact).
constexpr float kSandPressure = 1.4f;   // (sand is ~0.6 of the density of rock) x (lateral pressure coefficient ~ 0.25), boosted so a heap reads as a push
constexpr int kMaxColumn = 40;
constexpr int kReach = 3;
constexpr float kEvalAwake = 1.0f / 15.0f, kEvalAsleep = 0.25f;
constexpr float kWakeRound = 0.02f;   // a sleeping ball wakes when the sand pushes with more than this fraction of its weight
constexpr float kWakeBlock = 0.6f;    // anything else has to beat its friction first (else it would just wake, sit still and fall asleep again)
}  // namespace

// The player (moving at `speed` along `axis` in direction `sign`) is blocked by the slab of cells [lo, hi]. Every body in it
// is shoved: the heavier it is relative to the player the slower it is pushed away. Returns the speed the player can keep
// going at, or -1 when the slab holds no rigid body.
float Physics::pushFromPlayer(const IVec3& lo, const IVec3& hi, int axis, int sign, float speed) {
    Vec3 dir(0, 0, 0);
    dir[axis] = (float)sign;
    float cap = -1.0f;
    for (auto& up : bodies) {
        Body& b = *up;
        if (b.dead || b.count == 0) continue;
        float d2 = 0;
        const float c[3] = {b.pos.x, b.pos.y, b.pos.z};
        const int l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
        for (int i = 0; i < 3; i++) {
            const float mn = (float)l[i], mx = (float)h[i] + 1.0f;
            const float e = c[i] < mn ? mn - c[i] : (c[i] > mx ? c[i] - mx : 0.0f);
            d2 += e * e;
        }
        if (d2 > b.radius * b.radius) continue;
        Vec3 sum(0, 0, 0);
        int hits = 0;
        for (int z = lo.z; z <= hi.z; z++)
            for (int y = lo.y; y <= hi.y; y++)
                for (int x = lo.x; x <= hi.x; x++) {
                    if (!world.overlayAt(x, y, z)) continue;
                    const Vec3 ctr((float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f);
                    const Vec3 q = b.worldToLattice(ctr);
                    if (!b.occupiedAt(floorToInt(q.x), floorToInt(q.y), floorToInt(q.z))) continue;
                    sum += ctr;
                    hits++;
                }
        if (!hits) continue;
        const Vec3 r = sum * (1.0f / (float)hits) - b.pos;
        const float vb = dot(b.vel, dir);
        float vt = speed * kPlayerMass / (kPlayerMass + b.mass);
        vt = std::max(vt, std::min(speed, kCreepSpeed));
        if (vb < vt) {
            const Vec3 P = dir * ((vt - vb) * b.mass);  // impulse at the contact: a ball is set rolling, a tall thing tips
            b.vel += P * b.invMass;
            b.ang += b.invIworld * cross(r, P);
        }
        b.wake();
        const float keep = std::max(vt, vb);
        cap = cap < 0 ? keep : std::min(cap, keep);
    }
    return cap;
}

// Re-measures what the sand around a body pushes it with: every exposed side face with a sand voxel against it
// takes the pressure of the sand column above that voxel.
void Physics::evalSandPressure(Body& b) {
    b.sandF = b.sandT = Vec3(0, 0, 0);
    if (world.counts[M_SAND] == 0 || b.surf.empty()) return;
    const Mat3 R = b.rot.toMat3();
    const size_t n = b.surf.size();
    const size_t stride = n > 5000 ? (n + 4999) / 5000 : 1;
    static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    std::vector<uint64_t>& keys = sandKeys_;
    keys.clear();
    for (size_t i = 0; i < n; i += stride) {
        const Vec3 p = b.pos + R * b.surf[i];
        for (int d = 0; d < 4; d++) {
            // The sand within a few voxels pushes too (a heap that has just been pushed back slumps against the body again).
            for (int k = 1; k <= kReach; k++) {
                const int x = floorToInt(p.x + (float)(dirs[d][0] * k)), y = floorToInt(p.y), z = floorToInt(p.z + (float)(dirs[d][1] * k));
                const uint8_t m = world.get(x, y, z);
                if (m == M_AIR && !world.overlayAt(x, y, z)) continue;
                if (m == M_SAND && !world.overlayAt(x, y, z))
                    keys.push_back(((uint64_t)(uint32_t)(x + (1 << 20)) << 42) | ((uint64_t)(uint32_t)(z + (1 << 20)) << 21) | ((uint64_t)(uint32_t)y << 4) |
                                   ((uint64_t)(k - 1) << 2) | (uint64_t)d);
                break;
            }
        }
    }
    if (keys.empty()) return;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    // Small things are carried along by the sand much more readily than their weight alone says (grains wedge under and around
    // them, and a pebble hardly has a "static friction" worth the name against a heap): the pressure counts for more.
    const float boost = 1.0f + 12.0f / (1.0f + b.radius * 0.5f);
    Vec3 F(0, 0, 0), T(0, 0, 0);
    for (uint64_t k : keys) {
        const int d = (int)(k & 3), reach = (int)((k >> 2) & 3), y = (int)((k >> 4) & ((1u << 17) - 1));
        const int z = (int)((k >> 21) & ((1u << 21) - 1)) - (1 << 20), x = (int)((k >> 42) & ((1u << 21) - 1)) - (1 << 20);
        int h = 1;
        while (h < kMaxColumn && world.get(x, y + h, z) == M_SAND) h++;
        const Vec3 f(-(float)dirs[d][0], 0, -(float)dirs[d][1]);
        static const float kReachWeight[3] = {1.0f, 0.55f, 0.25f};
        const Vec3 force = f * (kSandPressure * boost * kReachWeight[reach] * (float)h * cfg::GRAVITY * (float)stride);
        const Vec3 at = Vec3((float)x + 0.5f - 0.5f * (float)dirs[d][0], (float)y + 0.5f, (float)z + 0.5f - 0.5f * (float)dirs[d][1]);
        F += force;
        T += cross(at - b.pos, force);
    }
    b.sandF = F;
    b.sandT = T;
}

// Called every physics step: refreshes the sand pressure of every body now and then (often while it moves, rarely while it
// sleeps) and pushes the awake ones with it.
void Physics::sandPressure(float dt) {
    if (world.counts[M_SAND] == 0) {
        for (auto& up : bodies) up->sandF = up->sandT = Vec3(0, 0, 0);
        return;
    }
    for (auto& up : bodies) {
        Body& b = *up;
        b.sandTimer += dt;
        if (b.sandTimer >= (b.asleep ? kEvalAsleep : kEvalAwake)) {
            b.sandTimer = 0;
            evalSandPressure(b);
            if (b.asleep && length(b.sandF) > (b.round ? kWakeRound : kWakeBlock) * b.mass * cfg::GRAVITY) b.wake();
        }
        if (b.asleep) continue;
        b.vel += b.sandF * (b.invMass * dt);
        b.ang += b.invIworld * (b.sandT * dt);
    }
}
