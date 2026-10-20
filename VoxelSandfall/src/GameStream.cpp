// Chunk loading, game side: when the player walks into a neighbouring region the voxel window recentres (World::shiftWindow)
// and everything that lives in window coordinates moves with it. See Stream.cpp for the world half.
#include <algorithm>

#include "Game.h"

namespace {
inline long long floorDiv(long long a, long long b) {
    long long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
inline bool insideWindow(const Vec3& p) {
    return p.x >= 0.0f && p.x < (float)cfg::WORLD_X && p.z >= 0.0f && p.z < (float)cfg::WORLD_Z;
}
}  // namespace

void Game::setInfinite(bool on) {
    infinite = on;
    reset();
}

// A body leaves the window: it is frozen and kept (its position relative to its region) until that region is loaded again.
void Game::stashBody(std::unique_ptr<Body> b) {
    const long long R = World::REGION;
    const long long gx = world.originX + (long long)std::floor(b->pos.x), gz = world.originZ + (long long)std::floor(b->pos.z);
    StashedBody s;
    s.rx = floorDiv(gx, R);
    s.rz = floorDiv(gz, R);
    b->pos -= Vec3((float)(s.rx * R - world.originX), 0.0f, (float)(s.rz * R - world.originZ));
    b->vel = b->ang = Vec3(0, 0, 0);
    b->asleep = true;
    b->ovCells.clear();
    b->ovValid = false;
    s.body = std::move(b);
    stash_.push_back(std::move(s));
}

void Game::restoreStashedBodies() {
    const long long R = World::REGION;
    for (size_t i = 0; i < stash_.size();) {
        StashedBody& s = stash_[i];
        if (!world.regionLoaded(s.rx, s.rz)) {
            i++;
            continue;
        }
        s.body->pos += Vec3((float)(s.rx * R - world.originX), 0.0f, (float)(s.rz * R - world.originZ));
        s.body->wake();  // it settles against whatever the loaded region contains now
        physics.bodies.push_back(std::move(s.body));
        stash_[i] = std::move(stash_.back());
        stash_.pop_back();
    }
}

// Recentres the window by whole regions (dRx, dRz) and moves everything with it.
void Game::shiftWorld(int dRx, int dRz) {
    const float dx = (float)(dRx * World::REGION), dz = (float)(dRz * World::REGION);
    physics.clearOverlays();  // (their raw indices refer to the old table)

    for (size_t i = physics.bodies.size(); i-- > 0;) {
        const Vec3 np = physics.bodies[i]->pos - Vec3(dx, 0, dz);
        if (insideWindow(np)) continue;
        stashBody(std::move(physics.bodies[i]));
        physics.bodies.erase(physics.bodies.begin() + (std::ptrdiff_t)i);
    }

    world.shiftWindow(dRx, dRz);

    for (auto& up : physics.bodies) {
        up->pos -= Vec3(dx, 0, dz);
        up->ovDirty = true;
        up->ovValid = false;
    }
    restoreStashedBodies();
    player.pos -= Vec3(dx, 0, dz);
    for (size_t i = 0; i < particles.size();) {
        particles[i].pos -= Vec3(dx, 0, dz);
        if (!insideWindow(particles[i].pos)) {
            particles[i] = particles.back();
            particles.pop_back();
        } else {
            i++;
        }
    }
    for (size_t i = 0; i < fx.size();) {  // flames and smoke move with the window
        fx[i].pos -= Vec3(dx, 0, dz);
        if (!insideWindow(fx[i].pos)) {
            fx[i] = fx.back();
            fx.pop_back();
        } else {
            i++;
        }
    }
    waterField.shift((int)dx, (int)dz);
    hollow_.clear();
    pendingShifts.push_back({dRx, dRz});
    regionsShifted++;
}

void Game::updateStreaming() {
    if (!infinite || !world.streaming) return;
    // bodies that wandered (or were thrown) out of the window are frozen into the store
    for (size_t i = physics.bodies.size(); i-- > 0;) {
        if (insideWindow(physics.bodies[i]->pos)) continue;
        stashBody(std::move(physics.bodies[i]));
        physics.bodies.erase(physics.bodies.begin() + (std::ptrdiff_t)i);
    }
    // The player stays within a few voxels of the middle region (so the edge of the loaded world is always far away); a little
    // hysteresis keeps standing on a border from flip-flopping.
    const float R = (float)World::REGION, margin = 6.0f;
    auto step = [&](float p) {
        const float lo = (float)World::CENTER * R, hi = (float)(World::CENTER + 1) * R;
        if (p >= lo - margin && p < hi + margin) return 0;
        return (int)std::floor(p / R) - World::CENTER;
    };
    const int dRx = std::clamp(step(player.pos.x), -4000, 4000), dRz = std::clamp(step(player.pos.z), -4000, 4000);
    if (dRx || dRz) shiftWorld(dRx, dRz);
}
