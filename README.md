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
| `1`-`9`, `0` (or `Q` / `E`) | Sand · Water · Normal Stone · Permanent Stone · Dirt · Grass · Coal · Fire · Smoke · Oil |
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

**Coal, oil, fire and smoke** (`FireWorld.cpp`, `GameFire.cpp`, `Fire.h`, `fx.hlsl`; keys `7` coal, `8` fire, `9` smoke, `0` oil):
* *Coal* is rock that burns. Like stone it is static where it is attached to the ground and becomes rigid bodies when it is cut loose, but
  coal painted in the air comes down as **pebbles** (the cloud is cut into irregular pieces like crushed rock salt, each one a rigid
  body that you can walk over, kick or shove). A flame touching coal lights it; burning coal stays on fire for 25 - 70 s and then
  is gone, voxel by voxel; it lights the coal next to it **a third less eagerly than a flame does** (rate x 2/3), so a fire creeps through a
  clump instead of flashing over it. Coal that is held up by what burns away comes loose and falls as rigid bodies; burning rocks
  burn voxel by voxel too and fall apart as they do. Coal with water next to it **cannot catch fire**, and burning coal that water reaches goes out.
* *Fire* sets things alight: flame particles rise from burning fuel (and from the Creator's brush), throw sparks, glow on what is around
  them (merged into up to 16 soft lights the shaders add to nearby surfaces), and die as steam in water. It is **drawn by a shader, not
  as sprites** (`firevol.hlsl`): the CPU only fills a coarse 3D heat field (one texel per 4 voxels: a column of heat above every
  exposed burning voxel, taller for oil, plus the flames in flight), and every pixel marches a ray through it, eroding the heat with
  tall noise that scrolls upwards (tongues of flame lick up, split and die) and lighting what is left with a black-body colour ramp. The ray
  stops at the scene's depth (resolved to one sample per pixel), so rocks and walls hide the flames behind them.
* *Smoke* rises slowly, drifts on slow currents, spreads out under ceilings and thins away in a few seconds; burning coal makes grey
  smoke, burning oil black soot, fire meeting water pale steam. The Creator paints puffs of it.
* *Oil* is a thick liquid: it flows like water but slowly, floats on water (water sinks through it), sand sinks through it, and it is
  dark and shiny with a rainbow film. Fire **sweeps through it as a wave**: it catches where a flame touches it and runs out through
  all the connected oil from there, layer by layer (40 - 320 voxels a second, faster for more oil, so any pool is alight in a second or
  two); two places where fire met the oil are two waves that run into each other, and the fresh front flares up taller than what has
  been burning for a while. It lights the coal it touches at once; each oil voxel burns for 10 - 30 s and is gone (burning oil does
  not flow; it drops if what is under it burns away).
  Oil under water cannot burn; water over burning oil puts it out.
* Burning voxels are tracked in `World::burning` (they move with the endless-world window and are saved and restored while burning).
  Smoke and sparks are camera-facing sprites (`Renderer::buildFx`, smoke sorted far to near, lit and fogged per corner). A fire that fills the screen
  from close by costs about 3-4 ms of GPU (fewer ray steps on the lower quality tiers); nothing is drawn or uploaded when nothing burns.

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

**Pushing rocks around** (`BodyForces.cpp`) - a free rock is a real body you can shove and that the sand shoves back:
* *The player pushes*: walking into a rock too tall to step on (the step-up is 6 voxels) shoves it. The player counts as ~5300 voxels of stone, so the
  push speed is the walking speed times 5300 / (5300 + the rock's voxels): a football (~900 voxels) rolls away almost as fast as you walk, a 20 cm rock
  is pushed slowly, a metre-wide boulder only creeps (~18 voxels/s minimum) and holds you back. Pebbles are low enough to be stepped over;
* *Round rocks roll*: a roughly spherical body collides as a true sphere (its surface voxels are pulled onto a sphere for world contacts) and has little
  rolling resistance, so a ball rolls down a sand slope and rolls away when pushed instead of tipping over the facets of its voxels. Blocky rocks slide / tip;
* *Sand pushes back*: sand heaped against a body presses on it with lateral earth pressure (the weight of the sand column above each contact, reaching
  3 voxels out so a heap that slumps back keeps pushing). Small things are carried along readily, a big boulder is not moved by a little sand (the
  pressure is measured every ~1/15 s, much less often while asleep); when a body moves it wakes the sand that it left behind so the heap slumps after it.

**Endless world / chunk loading** (`Stream.cpp`, `GameStream.cpp`) — the game starts in an endless world (`--arena` brings back the classic walled arena;
scripts and tests default to the arena, `--infinite` / script `infinite on` switch). The voxel grid is a window of 15 x 15 *regions* (one region = one
32 x 32 chunk column, full 256 height, so 480 x 480 voxels) around you, and it slides as you walk - you are always within a region of its middle, so the
loaded world reaches more than 200 voxels in every direction:
* nothing is copied - a 225-entry table maps the window's chunk columns to storage blocks, so sliding the window permutes the table; coordinates inside the
  window stay small (0..479) forever, so the simulation, physics, shaders etc. never see big numbers (the window's global origin is tracked separately);
* the regions that fall off the window are saved if you ever changed anything in them (run-length compressed, in memory; untouched ones cost nothing and
  are simply generated again), and their storage is reused for the regions that come into view, restored from the store or generated (a flat Permanent
  Stone floor for now). Water, sand, rigid bodies, grass and everything else you left there are exactly as you left them when you come back;
* the cellular sim and physics carry on across a slide (active lists are rebased, rigid bodies in unloaded regions are kept aside frozen, water that was
  held back by the old edge of the window flows into the new region);
* on the GPU the volume textures are copied (not re-uploaded) into the shifted position, retained chunk meshes are never rebuilt - each is drawn with a
  per-instance translation - and only the newly loaded row of regions is meshed and uploaded. A slide costs about 1.5 ms of CPU and nothing you can see;
* *Haze*: round distance fog from the eye (what you see is a dome, never the square of the window) - nothing for the first ~70 voxels, then a thin veil that
  thickens until everything is swallowed just before the window's edge, easing in with neither slope nor curvature at the end so there is no visible rim
  and nothing pops in or out. The haze is the colour of the sky in the direction you look, and below the horizon the "ground" is that same haze (darker
  and bluer the further down you look, close to the floor's own tone), so distant voxels melt into it;
* *Sky*: a deeper blue sky with soft drifting clouds (the light the sky casts is unchanged; the arena keeps its old plain sky and no fog).

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
  `prof` / `profreset` / `profiler on|off`, `cam x y z yawDeg pitchDeg`, `viewcam x y z yawDeg pitchDeg|off` (free camera), `look yaw pitch`, `walk|run seconds`, `fillsand` / `fillwater` / `fillstone x0 x1 y0 y1 z0 z1`, `dropwater x y z r` (ball of water), `dropstone x y z half` (rigid cube), `impacts on|off|nosoak`, `infinite on|off`, `tp globalX globalZ`, `filldirt x0 x1 y0 y1 z0 z1`, `growgrass x y z r`, `material dirt|grass`, `quality auto|0..4`, `dumpfield file.png x0 x1 z0 z1`, `crater speed`, `freeze on|off`, `tool creator|gun`, `material sand|water|stone|bedrock`, `brush N`, `paint [n]`,
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
| `FireWorld.cpp`, `GameFire.cpp`, `Fire.h` | coal / oil burning in the grid, flames, smoke and sparks, burning rocks, fire lights |
| `Stream.cpp`, `GameStream.cpp` | endless world: region window, storage table, region save / load / generation, recentring of the sim, player, bodies |
| `Config.h` | all tunables (arena size, gravity, speeds, tick rates) |

## Known limitations / ideas for later versions

Rigid-body physics is single-threaded; very large floating islands (100k+ voxels) cost several ms per physics step;
the endless world is a flat floor (no terrain generation yet) and its saved regions live in memory only (not on disk); rigid bodies do not push the player and do not float; no sound; water leveling is line-of-sight based, so an isolated 1-voxel bump can remain.
