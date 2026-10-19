# Voxel Sandfall — Version 0.0.1 (initial prototype)

A fully custom voxel engine written from scratch in C++20 for Visual Studio. **No third-party libraries**:
rendering is plain Direct3D 11 (ships with Windows), and the cellular simulation, rigid-body physics,
collision, destruction and island splitting are all hand-written (no Box2D/Box3D/PhysX/Bullet).

```
VoxelSandfall.sln
VoxelSandfall/
  VoxelSandfall.vcxproj      Visual Studio project (x64, Debug + Release)
  src/                       engine + game source
  shaders/                   HLSL (compiled at run time, hot-reload with F9)
```

## Build & run

* Requirements: Visual Studio 2026 (or 2022) with the **Desktop development with C++** workload and a Windows 10/11 SDK;
  a GPU with Direct3D 11 feature level 11.0.
* Open `VoxelSandfall.sln`, pick **x64 | Release** (or Debug) and press **F5**.
  The project targets toolset `v145` (VS 2026). On VS 2022 change *Platform Toolset* to `v143` (Project → Properties → General).
* Command line: `MSBuild VoxelSandfall.sln /p:Configuration=Release /p:Platform=x64`, then run `bin\Release\VoxelSandfall.exe`.
* Shaders are copied next to the exe on build and are also read from `VoxelSandfall\shaders` when you run from `bin\<config>\`,
  so you can edit an `.hlsl` file and press **F9** in the running game to reload it.

## Controls

| Input | Action |
|---|---|
| `W A S D`, `Space`, `Shift` | move, jump, sprint |
| Mouse | look (click the window to re-capture, `Esc` releases the mouse, `Esc` again quits) |
| `R` | **Creator tool** — place / attach voxels |
| `F` | **Gun tool** — AK-style rifle, hitscan, carves voxels (hold LMB for full auto) |
| `1` `2` `3` `4` (or `Q` / `E`) | Sand · Water · Normal Stone · Permanent Stone |
| Mouse wheel | brush radius (placement size / destruction size) |
| Left mouse | paint (Creator) / fire (Gun) |
| `[` `]` / `;` `'` | rotate the sun / raise-lower the sun |
| `F1` help · `F2` screenshot (`bin\<cfg>\shots`) · `F3` HUD · `F5` reset world · `F6` sky-ray quality · `F7` vsync · `F9` reload shaders · `F11` fullscreen | |

## What is implemented (spec 0.0.1)

**Materials**
* **Sand** — falling-sand cellular automaton: falls straight down, otherwise slides diagonally (±X / ±Z); never merges into bodies.
  Sand also sinks through water.
* **Water** — fluid automaton: falls, otherwise spreads into adjacent empty cells (L/R/F/B). It flows toward nearby drops,
  flattens columns under pressure and levels out into a thin sheet.
* **Normal Stone** — contiguous stone forms a rigid body (`Physics.cpp`): mass, inertia tensor, gravity, friction, sleeping,
  body-vs-world and body-vs-body collision (sequential-impulse solver with warm starting, one collision sphere per surface voxel).
* **Permanent Stone** — immovable, indestructible foundation (the bedrock floor is made of it); immune to the gun.

**Destruction & island splitting** — every shot removes a sphere of voxels (`carveSphere`), then a breadth-first 3D flood fill
(`detachIslands`) starts from the cells that touch the hole. Stone that is no longer connected to Permanent Stone becomes a new
rigid body that falls and tumbles. Shooting a rigid body removes voxels from it and re-labels its connected components, so it splits
into several bodies, each inheriting the velocity of its piece.

**Player** — 50 × 100 × 50 voxel box centred on the camera column, eye at 90 voxels, swept-AABB voxel collision with auto step-up.

**Tools** — Creator: wireframe brush sphere, attaches to the targeted face or paints in free space (at a fixed distance).
Gun: ray from the crosshair, red carve-radius preview, recoil + muzzle flash.

**Template world** — 256 × 256 × 1 Permanent Stone floor enclosed by 3-voxel-thick Normal Stone walls (160 voxels high).

**Lighting** — directional sun with a 4096² shadow map (3×3 PCF), plus *ray-traced sky light*: the world occupancy is mirrored in a 3D
texture and every pixel traces cosine-weighted hemisphere rays through the voxel grid (DDA) for sky visibility, and a short voxel-exact
ray toward the sun closes contact gaps. Carving a hole through a wall therefore lights the room with both direct sun and sky light.
Per-vertex ambient occlusion, 4× MSAA, water with Fresnel / sun glints.

## Interpretation notes (where the spec left room)

* **Anchoring** — Normal Stone that touches Permanent Stone (directly, or through other stone) lives in the static world grid and
  counts as the "main static structure"; the template walls are anchored this way. Stone painted onto an anchored surface joins it;
  stone painted in free space, or severed by the gun, becomes a dynamic rigid body. Stone painted against a rigid body merges into it.
* **Sand/water vs rigid bodies** — bodies rest on sand heaps; sand and water treat rigid bodies as obstacles. Water does not push or float bodies.
* The Creator never overwrites stone or Permanent Stone (stone replaces loose sand/water); the player's own box is kept clear.

## Performance notes

Measured on an RTX 5060 laptop at 1920x1080 (`F4` shows the live CPU/GPU breakdown; `prof` in a script logs it):

| Scenario | Before | After |
|---|---|---|
| 56k-voxel water/sand drop, cell sim per frame | 12-13 ms | 1-4 ms |
| same, GPU frame time | 12-16 ms | 2-3 ms |
| 463k-voxel brush (radius 48): painting | ~70 ms | 7-11 ms |
| 463k-voxel sand + 463k-voxel water in motion, CPU per frame | 35-55 ms | 10-12 ms |

What changed: the sand/water automaton runs on all cores (8x8 voxel tiles, 4 checkerboard phases, padded to avoid false
sharing), voxels fall up to 3 cells per tick, moves wake 9 neighbours instead of 27, bulk edits (paint/carve/island removal)
bypass per-voxel bookkeeping, water diffusion "cools down" so pools stop re-meshing, chunk meshing is greedy and parallel
(per-pixel AO from the occupancy volume), the sky ray march skips empty 8x8x8 bricks, the colour pass runs after a depth
pre-pass, the shadow map is only re-rendered when the scene changes, mesh buffers are reused, and the sim/mesh work per
frame is time-budgeted (a huge pour slows the simulation slightly instead of dropping the frame rate).
The `Debug` configuration is now optimised as well (a voxel engine is unusable at `/Od`); `Release` is still fastest.
`VS_THREADS=n` limits the worker thread count; `--rays 0` / `F6` disables ray-traced sky light; `F8` cycles MSAA.

## Tests and scripted runs

* `VoxelSandfall.exe --selftest` — headless regression tests (arena template, DDA ray casts, sand pile, water spread, player
  controller, rigid body stacking, island splitting, bodies on sand, …) → `selftest.log` next to the exe, exit code = failures.
* `VoxelSandfall.exe --script file.txt [--size 1600x900] [--msaa 1|2|4|8]` — drives the real renderer without a human. Commands:
  `prof` / `profreset` / `profiler on|off`, `cam x y z yawDeg pitchDeg`, `freeze on|off`, `tool creator|gun`, `material sand|water|stone|bedrock`, `brush N`, `paint [n]`,
  `fire [n]`, `carve x y z r`, `wait seconds`, `shot file.png`, `sun az el`, `rays n`, `hud on|off`, `log text`, `reset`, `quit`.
* Other flags: `--rays N` (sky ray count, 0 = off), `--novsync`, `--autoquit seconds`. Diagnostics go to `voxelsandfall.log`.

## Source map

| File | Purpose |
|---|---|
| `World.*`, `CellSim.cpp` | chunk-contiguous voxel grid, change tracking, DDA ray cast, sand/water automata (active-voxel list) |
| `Physics.*` | rigid bodies (voxel lattices), mass properties, collision detection, solver, sleeping, overlay for sand/player |
| `Tools.*` | scene ray casts, carve + flood-fill island detection, Creator painting/anchoring/merging |
| `Player.*`, `Game.*` | controller, per-frame update order, tool logic, debris particles |
| `Mesher.*` | greedy chunk meshing (parallel), per-vertex-AO meshing for rigid bodies, occupancy bricks |
| `Parallel.h`, `Profile.h` | fork-join thread pool, CPU/GPU frame profiler |
| `Renderer.*`, `ViewModel.*`, `Png.*` | Direct3D 11 renderer, procedural rifle, HUD, screenshots |
| `Config.h` | all tunables (arena size, gravity, speeds, tick rates) |

## Known limitations / ideas for later versions

Rigid-body physics is single-threaded; very large floating islands (100k+ voxels) cost several ms per physics step;
rigid bodies do not push the player or displace sand/water; no sound; water has no buoyancy.
