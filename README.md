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
| `W A S D`, `Space`, `Shift` | move, jump (swim up in deep water), sprint |
| Mouse | look (click the window to re-capture, `Esc` releases the mouse, `Esc` again quits) |
| `R` | **Creator tool** — place / attach voxels |
| `F` | **Gun tool** — AK-style rifle, hitscan, carves voxels (hold LMB for full auto) |
| `1` `2` `3` `4` `5` `6` (or `Q` / `E`) | Sand · Water · Normal Stone · Permanent Stone · Dirt · Grass |
| Mouse wheel | brush radius (placement size / destruction size) |
| Left mouse | paint (Creator) / fire (Gun) |
| `[` `]` / `;` `'` | rotate the sun / raise-lower the sun |
| `F1` help · `F2` screenshot (`bin\<cfg>\shots`) · `F3` HUD · `F5` reset world · `F6` sky-ray quality · `F7` vsync · `F9` reload shaders · `F12` graphics quality (auto, then tiers 0-4) · `F11` fullscreen | |

## What is implemented (spec 0.0.1)

**Materials**
* **Sand** — falling-sand cellular automaton: falls straight down, otherwise slides diagonally (±X / ±Z); never merges into bodies.
  Sand also sinks through water.
* **Water** — fluid automaton: falls straight down, otherwise spreads into adjacent empty cells (L/R/F/B). It then *finds its
  level*: a voxel at the top of a column looks along eight lines (axes and diagonals, up to 128 voxels, straight through other
  water) for a column at least two voxels lower and hops there, so a pool settles flat and fills its whole container (a basin
  ends up 100 % at one level, a long trench ~95 %). Water that has been falling and *lands* is an impact: on a pool it soaks in
  (it is placed on the lowest nearby surface instead of stacking like slime), some of it leaves as flying droplets, and the
  impact makes ripples and foam. Hops, soaks and droplets are proposed in the parallel phase and applied serially.
* **Dirt** — a mixture of stone and sand (`Dirt.cpp`, `CellSim.cpp`). Poured dirt falls like sand but is much clumpier (a grain only
  slides off a 3-5 voxel drop, so it makes steep heaps - about half the footprint of the same amount of sand) and then *settles*: static
  dirt keeps its shape like stone, steep walls and overhangs included. It is only re-checked when something next to it changes:
  a voxel with an unbroken column of dirt/grass down to something solid is supported; everything else hangs and is held only
  by the faces that tie it to the supported structure and to stone walls. A connected hanging mass holds while
  `voxels <= 14 x contact faces`, so a short thick shelf stays up, but a long thin arm, a big lump on a thin neck, or a piece that was cut
  loose breaks, and the whole hanging mass becomes loose dirt that falls and piles up again. Nothing is lost.
* **Grass** — a skin on dirt. It can only be painted onto settled dirt (the Creator's brush turns the dirt surface inside it to grass;
  it does nothing on air, stone, sand or deep inside dirt) and it lives at most two voxels deep below the open surface (tops and
  sides, not the underside of an overhang). It spreads by itself, slowly, over every exposed dirt voxel in reach, dies back to dirt when
  it is buried, and turns into dirt when it falls.
* **Normal Stone** — contiguous stone forms a rigid body (`Physics.cpp`): mass, inertia tensor, gravity, friction, sleeping,
  body-vs-world and body-vs-body collision (sequential-impulse solver with warm starting, one collision sphere per surface voxel).
* **Permanent Stone** — immovable, indestructible foundation (the bedrock floor is made of it); immune to the gun.

**Player <-> sand and water** (`SandFx.cpp`) — sand and water react to the player; stone does not (it is rigid). The harder or
faster you hit the surface, the bigger the reaction. Displaced material is conserved: it is thrown out and lands again as real
voxels that then obey the normal sand / water rules, so ridges slump, dents fill, wakes close and splashes run back downhill.
* *sand footsteps* — each step digs a dent (scaled by speed) and kicks grains backwards and sideways into a berm;
  a steady dust spray comes off the surface while you run;
* *hard landings* — falling or jumping onto sand digs a crater with a rim and sprays grains outwards;
* *wading through sand* — gentle slopes are climbed with the normal step-up, but a pile that is too steep to climb (up to 20
  voxels high) is shoved aside to both flanks as you walk into it; taller or non-sand obstacles still block;
* *water is not ground* — the player's torso is solid to water, so you wade, swim and splash instead of walking on it:
  water inside the body is pushed out as a bow wave and spray, and the pool levels itself back in behind you. Plunging in from a
  height throws up a crown of droplets, wading is slower, `Space` swims up, and you float with your head above water in deep pools.
**Wading is cheap on purpose:** the deep water that the front of the player's body pushes aside is carried straight to the cells the
back of the body just left (no pit that has to cascade shut, no bow wave under the surface); only the voxels at the surface are thrown
out as droplets / splash, and falling water near the player does not count as a surface impact. Walking through a 1M-voxel pool
costs about 2-3 ms of CPU per frame at a run (it was ~10 ms).
**Rigid bodies** that fall or move into water throw it out the same way (stone stays inert against sand): the harder a block hits the
surface, the bigger the bow wave, crown of droplets and ripples; the displaced water is never lost.
Flying grains/droplets are small cubes (up to 6000 at once); only a small share of a footprint is "compacted" and disappears.

**Water surface** (`WaterField.*`, `PSWater` in `world.hlsl`) — purely cosmetic, never touches the voxel grid:
* a 2D wave field over the world columns: splashes, landing droplets, the wading player and falling stone make ripples that
  spread, reflect off the pool edges and die away; foam is left behind and fades; the stair-stepped voxel surface is blurred into
  a smooth one (no terraces) and shore proximity is stored for the foam line along banks;
* the surface is shaded with a smooth normal (ripples + calm travelling waves), a Fresnel mix of the mirrored sky / walls (a
  voxel-exact reflection ray) and the water body, which gets bluer and darker with depth (red is absorbed first), sun glints,
  animated caustics on the bed, foam, and a blue-green veil when the camera is under water.

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
* **Sand/water vs rigid bodies** — bodies rest on sand heaps; sand and water treat rigid bodies and the player's torso as obstacles. Bodies displace and splash water as they enter it, but water does not float or push them.
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
Water got a second optimisation pass: chunks where only water moved rebuild just their water mesh (every other frame) and no
longer invalidate the sun shadow map, water droplets do not trigger shadow re-renders, soaking is capped per tick and its search
is short, the ripple field only simulates / re-smooths / uploads the tiles that changed, and the water shader skips caustics, foam
and sub-pixel waves where they cannot be seen. A 463k-voxel pour dropped from ~16 ms to ~10 ms of CPU per frame, and a screen full of
water renders about 2x faster.
**Quality tiers** (`F12`, `--quality auto|0..4`, script `quality`): the sky-light rays are by far the most expensive part of a frame, so
quality is a ladder - tier 4: 8 rays, full resolution, full water; tier 3: 6 rays; tier 2: 4 rays, 88 % resolution, medium water;
tier 1: 3 rays, 78 %, simple water, 2x MSAA, 2048 shadow map; tier 0: 2 rays, 67 %, 1x MSAA. Below 100 % the scene is rendered
smaller and upscaled with a light sharpen (the HUD stays sharp). The default is tier 4 - the full-detail look (8 rays at every distance, 9-tap shadows, contact shadows, full water). *Auto* (`F12`,
off by default) measures the larger of CPU work and GPU time and steps down when it stays over ~9 ms, back up when there is
headroom; only the low tiers also use fewer rays on far surfaces. GPU timings are now collected from a ring of queries (the GPU runs frames behind), so `F4` and `prof` show real numbers.
Rigid bodies: a coarse solidity map skips voxels with nothing solid nearby, overlays of barely-moving bodies are kept, and bodies get a
4 ms budget per frame (beyond it they run in slow motion), which took a stack of three 30-40 voxel blocks from ~26 ms to ~7 ms.
Chunk rebuilds are done in small slices so a big edit (a stone tower) spreads over a few frames instead of one long hitch.
`VS_THREADS=n` limits the worker thread count; `--rays 0` / `F6` disables ray-traced sky light; `F8` cycles MSAA.

## Tests and scripted runs

* `VoxelSandfall.exe --selftest` — headless regression tests (arena template, DDA ray casts, sand pile, water spread, player
  controller, sand footprints / craters / wading, water leveling in basins and trenches, wading / plunging / floating in water, rigid body stacking, island splitting, bodies on sand, …) → `selftest.log` next to the exe, exit code = failures.
* `VoxelSandfall.exe --script file.txt [--size 1600x900] [--msaa 1|2|4|8]` — drives the real renderer without a human. Commands:
  `prof` / `profreset` / `profiler on|off`, `cam x y z yawDeg pitchDeg`, `viewcam x y z yawDeg pitchDeg|off` (free camera), `look yaw pitch`, `walk|run seconds`, `fillsand` / `fillwater` / `fillstone x0 x1 y0 y1 z0 z1`, `dropwater x y z r` (ball of water), `dropstone x y z half` (rigid cube), `impacts on|off|nosoak`, `filldirt x0 x1 y0 y1 z0 z1`, `growgrass x y z r`, `material dirt|grass`, `quality auto|0..4`, `dumpfield file.png x0 x1 z0 z1`, `crater speed`, `freeze on|off`, `tool creator|gun`, `material sand|water|stone|bedrock`, `brush N`, `paint [n]`,
  `fire [n]`, `carve x y z r`, `wait seconds`, `shot file.png`, `sun az el`, `rays n`, `hud on|off`, `log text`, `reset`, `quit`.
* Other flags: `--rays N` (sky ray count, 0 = off), `--novsync`, `--autoquit seconds`. Diagnostics go to `voxelsandfall.log`.

## Source map

| File | Purpose |
|---|---|
| `World.*`, `CellSim.cpp` | chunk-contiguous voxel grid, change tracking, DDA ray cast, sand/water automata (active-voxel list) |
| `Physics.*` | rigid bodies (voxel lattices), mass properties, collision detection, solver, sleeping, overlay for sand/player |
| `Tools.*` | scene ray casts, carve + flood-fill island detection, Creator painting/anchoring/merging |
| `Player.*`, `Game.*`, `SandFx.cpp` | controller, per-frame update order, tool logic, debris particles, sand interaction |
| `Mesher.*` | greedy chunk meshing (parallel), per-vertex-AO meshing for rigid bodies, occupancy bricks |
| `Parallel.h`, `Profile.h` | fork-join thread pool, CPU/GPU frame profiler |
| `Renderer.*`, `ViewModel.*`, `Png.*` | Direct3D 11 renderer, procedural rifle, HUD, screenshots |
| `Config.h` | all tunables (arena size, gravity, speeds, tick rates) |

## Known limitations / ideas for later versions

Rigid-body physics is single-threaded; very large floating islands (100k+ voxels) cost several ms per physics step;
rigid bodies do not push the player and do not float; no sound; water leveling is line-of-sight based, so an isolated 1-voxel bump can remain.
