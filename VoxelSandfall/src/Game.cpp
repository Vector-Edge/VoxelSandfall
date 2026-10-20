#include "Game.h"
#include "Profile.h"

Game::Game() {
    reset();
    player.plowHook = [this](const IVec3& mn, const IVec3& mx, int axis, int sign) { return sandPlow(mn, mx, axis, sign); };
}

void Game::reset() {
    physics.bodies.clear();
    stash_.clear();
    pendingShifts.clear();
    physics.nextId = 1;
    if (infinite) world.initStreaming();
    else world.generateArena();
    world.clearPlayerBox();
    if (infinite) player.respawnAt(Vec3(cfg::WORLD_X * 0.5f, 1.0f, cfg::WORLD_Z * 0.5f));
    else player.respawn();
    particles.clear();
    hollow_.clear();
    world.splashes.clear();
    waterField.reset();
    physAcc_ = simAcc_ = paintTimer_ = 0;
}

void Game::update(float dt, const GameInput& in) {
    dt = std::min(dt, 0.1f);
    time += dt;

    {
        prof::Scope sc(prof::P_STREAM);
        updateStreaming();
    }

    // --- look / tool selection -------------------------------------------------------------
    player.yaw -= in.lookDX * mouseSens;
    player.pitch = clampf(player.pitch - in.lookDY * mouseSens, -1.55f, 1.55f);
    if (in.selectCreator) tool = TOOL_CREATOR;
    if (in.selectGun) tool = TOOL_GUN;
    if (in.selectMaterial >= 1 && in.selectMaterial <= kPaintMaterialCount) materialIndex = in.selectMaterial - 1;
    if (in.cycleMaterial) materialIndex = (materialIndex + kPaintMaterialCount + in.cycleMaterial) % kPaintMaterialCount;
    brush = std::clamp(brush + in.wheel, cfg::BRUSH_MIN, cfg::BRUSH_MAX);

    // --- player ------------------------------------------------------------------------------
    PlayerInput pi;
    pi.forward = in.forward;
    pi.strafe = in.strafe;
    pi.jump = in.jump;
    pi.sprint = in.sprint;
    {
        prof::Scope sc(prof::P_PLAYER);
        const Vec3 velBefore = player.vel;
        const float velYBefore = player.vel.y;
        const bool wasOnGround = player.onGround;
        plowParticleBudget_ = 60;
        player.update(world, dt, pi);
        updateSandInteraction(dt, in, velYBefore, wasOnGround);
        updateWaterInteraction(dt, velBefore);
        if (world.boxOn) waterField.setExclusion(world.boxMin[0], world.boxMin[2], world.boxMax[0], world.boxMax[2]);
        else waterField.clearExclusion();
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
        const double tp0 = prof::nowMs();
        while (physAcc_ >= pdt && steps < cfg::PHYS_MAX_STEPS) {
            physics.step(pdt);
            physAcc_ -= pdt;
            steps++;
            if (prof::nowMs() - tp0 > cfg::PHYS_BUDGET_MS) {  // overloaded: slow motion instead of a slow frame
                physAcc_ = 0;
                break;
            }
        }
        if (steps == cfg::PHYS_MAX_STEPS) physAcc_ = 0;
    }
    {
        prof::Scope sc(prof::P_OVERLAY);
        physics.rasterizeOverlay();
        updateBodySplashes();
    }

    // --- cellular automata (fixed tick rate) ----------------------------------------------------
    simAcc_ += dt;
    const float sdt = 1.0f / cfg::CELL_TICK_HZ;
    int ticks = 0;
    {
        prof::Scope sc(prof::P_SIM);
        const double t0 = prof::nowMs();
        while (simAcc_ >= sdt && ticks < cfg::CELL_MAX_TICKS) {
            // Splashing water may turn into flying droplets as long as there is room for them.
            world.dropletBudget = std::clamp(1500 - (int)particles.size(), 0, 40);
            world.simTick(rng);
            world.dropletBudget = 0;
            processSplashes();
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
        waterField.step(dt);
    }
}

PaintResult Game::paintAtCrosshair() {
    const Vec3 eye = player.eye(), dir = player.forwardDir();
    const float r = (float)brush;
    SceneHit h = castScene(world, physics, eye, dir, cfg::REACH, kGridSolidMask);
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
    SceneHit h = castScene(world, physics, eye, dir, cfg::REACH * 3, kGridSolidMask | matBit(M_WATER));
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
        SceneHit h = castScene(world, physics, eye, dir, cfg::REACH, kGridSolidMask);
        preview.valid = true;
        preview.onSurface = h.hit;
        preview.radius = r;
        preview.center = h.hit ? h.point + h.normal * (r * 0.5f + 0.25f) : eye + dir * cfg::FREE_PAINT_DIST;

        const bool continuous = (mat == M_SAND || mat == M_WATER || mat == M_DIRT || mat == M_GRASS);
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
        SceneHit h = castScene(world, physics, eye, dir, cfg::REACH * 3, kGridSolidMask | matBit(M_WATER));
        preview.valid = h.hit;
        preview.onSurface = h.hit;
        preview.radius = (float)brush;
        preview.center = h.point + dir * 0.3f;
        if (in.fireHeld && gunCooldown <= 0) fireAtCrosshair();
    }
}

void Game::spawnDebris(const CarveResult& r) {
    for (size_t i = 0; i < r.debris.size(); i++) {
        if (particles.size() > (size_t)cfg::MAX_PARTICLES) break;
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
        bool remove = false;
        if (p.grain) {
            // Ballistic flight in sub-voxel steps; on contact the grain becomes a sand voxel in the last free cell.
            p.vel.y -= cfg::GRAVITY * dt;
            const float dist = length(p.vel) * dt;
            const int n = std::clamp((int)std::ceil(dist / 0.8f), 1, 10);
            const Vec3 step = p.vel * (dt / (float)n);
            bool landed = false, hitWater = false;
            for (int k = 0; k < n; k++) {
                const Vec3 np = p.pos + step;
                const int cx = floorToInt(np.x), cy = floorToInt(np.y), cz = floorToInt(np.z);
                // Droplets land on the water surface; sand grains sink through water to whatever is solid.
                if (world.playerSolid(cx, cy, cz) || (p.mat == M_WATER && world.get(cx, cy, cz) != M_AIR)) {
                    landed = true;
                    hitWater = p.mat == M_WATER && world.get(cx, cy, cz) == M_WATER;
                    break;
                }
                p.pos = np;
            }
            if (landed || p.life <= 0) {
                if (hitWater) waterField.impulse(p.pos.x, p.pos.z, 0.04f + length(p.vel) * (1.0f / 1400.0f), 1.1f, 0.12f);
                settleGrain(p.pos, p.mat);
                remove = true;
            } else if (p.pos.y < -20.0f || !world.inBounds(floorToInt(p.pos.x), 0, floorToInt(p.pos.z))) {
                remove = true;  // flew out of the arena
            }
        } else {
            p.vel.y -= cfg::GRAVITY * 0.8f * dt;
            Vec3 np = p.pos + p.vel * dt;
            if (p.mat == M_WATER && world.get(floorToInt(np.x), floorToInt(np.y), floorToInt(np.z)) == M_WATER) {
                waterField.impulse(np.x, np.z, 0.025f, 0.9f, 0.04f);  // spray falling back into the pool
                particles[i] = particles.back();
                particles.pop_back();
                continue;
            }
            if (world.playerSolid(floorToInt(np.x), floorToInt(np.y), floorToInt(np.z))) {
                // cheap bounce: test each axis separately
                if (world.playerSolid(floorToInt(np.x), floorToInt(p.pos.y), floorToInt(p.pos.z))) p.vel.x *= -0.35f; else p.pos.x = np.x;
                if (world.playerSolid(floorToInt(p.pos.x), floorToInt(np.y), floorToInt(p.pos.z))) p.vel.y *= -0.3f, p.vel.x *= 0.7f, p.vel.z *= 0.7f; else p.pos.y = np.y;
                if (world.playerSolid(floorToInt(p.pos.x), floorToInt(p.pos.y), floorToInt(np.z))) p.vel.z *= -0.35f; else p.pos.z = np.z;
            } else {
                p.pos = np;
            }
            remove = p.life <= 0 || p.pos.y < -50.0f;
        }
        if (remove) {
            particles[i] = particles.back();
            particles.pop_back();
        } else {
            i++;
        }
    }
}
