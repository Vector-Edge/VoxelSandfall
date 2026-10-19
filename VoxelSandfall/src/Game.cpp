#include "Game.h"
#include "Profile.h"

Game::Game() { reset(); }

void Game::reset() {
    physics.bodies.clear();
    physics.nextId = 1;
    world.generateArena();
    player.respawn();
    particles.clear();
    physAcc_ = simAcc_ = paintTimer_ = 0;
}

void Game::update(float dt, const GameInput& in) {
    dt = std::min(dt, 0.1f);
    time += dt;

    // --- look / tool selection -------------------------------------------------------------
    player.yaw -= in.lookDX * mouseSens;
    player.pitch = clampf(player.pitch - in.lookDY * mouseSens, -1.55f, 1.55f);
    if (in.selectCreator) tool = TOOL_CREATOR;
    if (in.selectGun) tool = TOOL_GUN;
    if (in.selectMaterial >= 1 && in.selectMaterial <= 4) materialIndex = in.selectMaterial - 1;
    if (in.cycleMaterial) materialIndex = (materialIndex + 4 + in.cycleMaterial) % 4;
    brush = std::clamp(brush + in.wheel, cfg::BRUSH_MIN, cfg::BRUSH_MAX);

    // --- player ------------------------------------------------------------------------------
    PlayerInput pi;
    pi.forward = in.forward;
    pi.strafe = in.strafe;
    pi.jump = in.jump;
    pi.sprint = in.sprint;
    {
        prof::Scope sc(prof::P_PLAYER);
        player.update(world, dt, pi);
    }

    // --- tools --------------------------------------------------------------------------------
    {
        prof::Scope sc(prof::P_TOOLS);
        updateTools(dt, in);
    }

    // --- rigid bodies (fixed step) ----------------------------------------------------------------
    physAcc_ += dt;
    const float pdt = 1.0f / cfg::PHYS_HZ;
    int steps = 0;
    {
        prof::Scope sc(prof::P_PHYS);
        while (physAcc_ >= pdt && steps < cfg::PHYS_MAX_STEPS) {
            physics.step(pdt);
            physAcc_ -= pdt;
            steps++;
        }
        if (steps == cfg::PHYS_MAX_STEPS) physAcc_ = 0;
    }
    {
        prof::Scope sc(prof::P_OVERLAY);
        physics.rasterizeOverlay();
    }

    // --- cellular automata (fixed tick rate) ----------------------------------------------------
    simAcc_ += dt;
    const float sdt = 1.0f / cfg::CELL_TICK_HZ;
    int ticks = 0;
    {
        prof::Scope sc(prof::P_SIM);
        const double t0 = prof::nowMs();
        while (simAcc_ >= sdt && ticks < cfg::CELL_MAX_TICKS) {
            world.simTick(rng);
            simAcc_ -= sdt;
            ticks++;
            if (prof::nowMs() - t0 > cfg::SIM_BUDGET_MS) break;  // keep the frame rate; the sim catches up later
        }
        if (simAcc_ > 2.0f * sdt) simAcc_ = 2.0f * sdt;           // never build up a backlog
    }
    lastSimTicks = ticks;

    {
        prof::Scope sc(prof::P_PARTICLES);
        updateParticles(dt);
    }
}

PaintResult Game::paintAtCrosshair() {
    const Vec3 eye = player.eye(), dir = player.forwardDir();
    const float r = (float)brush;
    SceneHit h = castScene(world, physics, eye, dir, cfg::REACH, matBit(M_SAND) | matBit(M_STONE) | matBit(M_BEDROCK));
    Vec3 center = h.hit ? h.point + h.normal * (r * 0.5f + 0.25f) : eye + dir * cfg::FREE_PAINT_DIST;
    Vec3 pmin, pmax;
    player.aabb(pmin, pmax);
    PaintResult res = paintSphere(world, physics, center, r, currentMaterial(), pmin, pmax);
    lastPlaced = res.placed;
    return res;
}

CarveResult Game::fireAtCrosshair() {
    const Vec3 eye = player.eye(), dir = player.forwardDir();
    gunCooldown = 60.0f / cfg::GUN_RPM;
    recoil = 1.0f;
    muzzleFlash = 0.06f;
    shotsFired++;
    CarveResult res;
    SceneHit h = castScene(world, physics, eye, dir, cfg::REACH * 3, matBit(M_SAND) | matBit(M_WATER) | matBit(M_STONE) | matBit(M_BEDROCK));
    if (h.hit) {
        res = carveSphere(world, physics, h.point + dir * 0.3f, (float)brush, rng);
        spawnDebris(res);
    }
    lastCarve = res;
    return res;
}

void Game::updateTools(float dt, const GameInput& in) {
    const Vec3 eye = player.eye(), dir = player.forwardDir();
    gunCooldown = std::max(0.0f, gunCooldown - dt);
    recoil = std::max(0.0f, recoil - dt * 9.0f);
    muzzleFlash = std::max(0.0f, muzzleFlash - dt);
    paintTimer_ = std::max(0.0f, paintTimer_ - dt);

    preview.valid = false;
    if (tool == TOOL_CREATOR) {
        const uint8_t mat = currentMaterial();
        const float r = (float)brush;
        SceneHit h = castScene(world, physics, eye, dir, cfg::REACH, matBit(M_SAND) | matBit(M_STONE) | matBit(M_BEDROCK));
        preview.valid = true;
        preview.onSurface = h.hit;
        preview.radius = r;
        preview.center = h.hit ? h.point + h.normal * (r * 0.5f + 0.25f) : eye + dir * cfg::FREE_PAINT_DIST;

        const bool continuous = (mat == M_SAND || mat == M_WATER);
        bool doPaint = false;
        if (continuous)
            doPaint = in.fireHeld && paintTimer_ <= 0;
        else
            doPaint = in.fireClicked || (in.fireHeld && paintTimer_ <= 0);
        if (doPaint) {
            PaintResult pr = paintAtCrosshair();
            // Big brushes are rate limited by the volume they dump (about 250k voxels per second while held).
            const float base = continuous ? 1.0f / 40.0f : 0.2f;
            paintTimer_ = std::max(base, (float)pr.placed / 250000.0f);
        }
    } else {
        SceneHit h = castScene(world, physics, eye, dir, cfg::REACH * 3, matBit(M_SAND) | matBit(M_WATER) | matBit(M_STONE) | matBit(M_BEDROCK));
        preview.valid = h.hit;
        preview.onSurface = h.hit;
        preview.radius = (float)brush;
        preview.center = h.point + dir * 0.3f;
        if (in.fireHeld && gunCooldown <= 0) fireAtCrosshair();
    }
}

void Game::spawnDebris(const CarveResult& r) {
    for (size_t i = 0; i < r.debris.size(); i++) {
        if (particles.size() > 700) break;
        Particle p;
        p.pos = r.debris[i];
        p.mat = r.debrisMat[i];
        Vec3 d(rng.range(-1, 1), rng.range(-0.2f, 1.2f), rng.range(-1, 1));
        p.vel = normalize(d) * rng.range(40, 160);
        p.life = rng.range(0.8f, 1.8f);
        p.size = rng.range(0.5f, 1.1f);
        particles.push_back(p);
    }
}

void Game::updateParticles(float dt) {
    for (size_t i = 0; i < particles.size();) {
        Particle& p = particles[i];
        p.life -= dt;
        p.vel.y -= cfg::GRAVITY * 0.8f * dt;
        Vec3 np = p.pos + p.vel * dt;
        if (world.playerSolid(floorToInt(np.x), floorToInt(np.y), floorToInt(np.z))) {
            // cheap bounce: test each axis separately
            if (world.playerSolid(floorToInt(np.x), floorToInt(p.pos.y), floorToInt(p.pos.z))) p.vel.x *= -0.35f; else p.pos.x = np.x;
            if (world.playerSolid(floorToInt(p.pos.x), floorToInt(np.y), floorToInt(p.pos.z))) p.vel.y *= -0.3f, p.vel.x *= 0.7f, p.vel.z *= 0.7f; else p.pos.y = np.y;
            if (world.playerSolid(floorToInt(p.pos.x), floorToInt(p.pos.y), floorToInt(np.z))) p.vel.z *= -0.35f; else p.pos.z = np.z;
        } else {
            p.pos = np;
        }
        if (p.life <= 0 || p.pos.y < -50.0f) {
            particles[i] = particles.back();
            particles.pop_back();
        } else {
            i++;
        }
    }
}
