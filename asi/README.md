# LibertyCraft.asi (GTA IV plugin)

The GTA IV side of LibertyCraft: a 32-bit Windows DLL with the `.asi` extension, loaded by
Ultimate ASI Loader from `<gamedir>/plugins/`. It talks to the Minecraft Fabric mod through the
shared-memory protocol in `../protocol/libertycraft_protocol.h` (SkyCraft v11 layout) and lets
Minecraft drive Niko and the camera.

Supported game: GTA IV **1.0.8.0** (1.0.7.0 should work too; anything else is detected and the
plugin stays inactive). Built on [IV-SDK](extern/iv-sdk) (GPL-3.0, git submodule).

## Build (Linux, cross-compiled)

Needs `clang-cl`, `lld-link`, `llvm-rc` (LLVM), `cmake` >= 3.21, `ninja`, and an
[xwin](https://github.com/Jake-Shadle/xwin) sysroot (Microsoft CRT + Windows SDK) at
`<repo>/.tools/xwin` (or set `XWIN_DIR`):

    xwin --accept-license --cache-dir ~/.xwin-cache --arch x86 --arch x86_64 splat --output .tools/xwin

Then (presets are looked up in the current directory, so run them from `asi/`):

    cd asi && cmake --preset release && cmake --build --preset release
    # -> asi/build/release/LibertyCraft.asi (+ LibertyCraft.pdb)
    # from the repo root: cmake --preset release -S asi && cmake --build asi/build/release

`--preset debug` builds `asi/build/debug/`. Copy the `.asi` to
`<gamedir>/plugins/LibertyCraft.asi` (`tools/build.sh` / `tools/launch.sh` do this).

Toolchain (`cmake/clang-cl-xwin.cmake`): `clang-cl --target=i686-pc-windows-msvc /vctoolsdir
<xwin>/crt /winsdkdir <xwin>/sdk`, `/std:c++17 /MT /EHsc /bigobj /W3 /Zi`, `lld-link /machine:x86
/dll /safeseh:no /debug:full`. `/MT` links the CRT statically: the DLL imports only KERNEL32,
USER32 and VERSION. `/safeseh:no` because IV-SDK's hooks are naked functions with inline asm.

IV-SDK quirks handled by the build:

- Its headers define non-inline globals, so it is included by exactly one translation unit,
  `src/dllmain.cpp`, which also `#include`s the SDK-dependent sources (`Game`, `Input`,
  `Collision`, `Render`, `Overlay`) unity-style. `Log`, `Config`, `Link`, `CrashLog` are normal TUs.
- `IVSDK.cpp` includes `<d3dx9.h>` but uses no D3DX: `src/compat/d3dx9.h` stands in for it.
- One construct clang rejects (`*reinterpret_cast<T*>(nullptr)`) is rewritten in a copy of the
  headers made at configure time (`build/*/ivsdk-patched/`); the submodule stays pristine.

## Tests (Linux)

The ring/seqlock core (`src/LinkCore.h`), the coordinate helpers (`src/Coords.h`) and the
vehicle / Niko-mode state machine (`src/DriveLogic.h`) are portable:

    cmake -S asi/tests -B asi/build/tests && cmake --build asi/build/tests && ctest --test-dir asi/build/tests

`tools/fake_minecraft.py` plays the Minecraft side against the real plugin (or any host):

    python3 tools/fake_minecraft.py --circle 3 --demo-section

It acknowledges teleports, walks a circle, prints every input event and a collision summary, and
with `--demo-section` writes an atlas + a few cubes into the render ring.

## Files

| file | what |
|---|---|
| `dllmain.cpp` | the IV-SDK translation unit; registers the hooks, logs the game version |
| `Link.*`, `LinkCore.h` | the shared mapping: transport, seqlocks, rings, overlay swap, heartbeat thread |
| `Game.*` | per frame: SkyState, McState, teleport handshake, mouse look, puppet mode, camera |
| `Input.*` | window subclass, raw mouse, DIK -> SDL3 scancodes, CPad zeroing |
| `Collision.*`, `collision/*` | v2: GTA's static collision sampled with one-sided line probes (`CWorld::ProcessLineOfSight`) per 8x8-block column: floors, ceilings and walls -> kColTris / kColRegion (region scheduler from SkyCraft); the water grid. `collision/Geometry.h` is SDK-free and tested on Linux (`tests/collision_test.cpp`). Street furniture (lamp posts, bins, benches, hydrants: GTA *objects*) in the 5x5 columns around the player: each object probed once with OBJECTS-only probes that skip everything else, as oriented boxes merged into the column's regions; doors, tiny and attached objects are left out, moved objects are re-probed once still (`collision/Objects.h`, SDK-free, tested; logged as `collision objects 10s`) |
| `Render.*` | `drawingEvent`: snapshots the frame's camera (render phase grcViewport / final cam), clock and flags into a draw command of our own that GTA's render thread executes; drains the render ring there |
| `render/World.*` | the D3D9 block renderer: atlas + mips, section vertex buffers, entities/scene/avatar, outline and cracks, Minecraft lighting, depth-tested against GTA's (FusionFix log) depth |
| `render/RenderMath.h`, `render/Shaders.h`, `render/Frame.h`, `render/D3D9Util.h` | pure helpers (tested on Linux), the HLSL (compiled at runtime by d3dcompiler_47), the frame snapshot, D3D9 helpers |
| `Overlay.*` | Minecraft's GUI/HUD composited over the frame (premultiplied alpha, crosshair invert pass, cursor) |
| `HostDrive.*`, `DriveLogic.h` | vehicles and Niko mode: who drives the player (Minecraft or GTA IV) |
| `Coords.h` | GTA <-> MC coordinates, heading <-> yaw, camera basis |
| `Combat.*`, `combat/CombatMath.h` | actor table (peds, and vehicles as pieces along their length -> Minecraft stand-ins), Minecraft hits/explosions/death -> GTA (vehicle hits: body/engine damage, windows, fire; arrows also hit the people inside), GTA damage to the puppeted player -> `kInHurt` |
| `Doors.*` | GTA's doors swing open for the Minecraft player (Niko is frozen with collision off, so he never pushes one): a door (object pool, `collision/Objects.h`'s door shape) opens away from the player when they walk into it, through GTA's door state (`SET_STATE_OF_CLOSEST_DOOR_OF_TYPE`; object heading or a push as fallbacks), and is shut and handed back to GTA once they are clear |
| `NpcBlocks.*`, `combat/BlockPush.h` | Minecraft's blocks are solid for GTA's peds and vehicles: `kRenSolids` bitsets (handed over by `render/World.cpp`'s ring consumer) push peds out of block columns (they follow walls round) and take the speed into blocks off cars and bikes. `BlockPush.h` is SDK-free and tested on Linux (`tests/combat_test.cpp`) |
| `Config.*`, `Log.*`, `CrashLog.*`, `Perf.h` | ini, log file, crash handler, frame-time stats |

Hooks: `processScriptsEvent` -> `Game::Tick` (natives), `processCameraEvent` -> `Game::Camera`
(after the final camera is written), `processPadEvent` -> `Input::Pad`, `drawingEvent` ->
`Render::Draw`, `ingameStartupEvent` -> `Game::OnIngameStartup`. A background thread beats the
host heartbeat every 250 ms (through loading screens, the pause menu and alt-tab) and reopens
the bridge file if Minecraft deleted/replaced it.

## Rendering (Render, render/, Overlay)

GTA IV records each frame's draw commands on the game thread and executes them on its render
thread, so Direct3D must not be touched from `drawingEvent` (CRenderPhasePostRenderViewport's
draw-list build, twice per frame). `Render::Draw` instead allocates a draw command of our own in
that draw list: the game's `CDrawRectDC` constructor writes the header (id/size word), then the
vtable is swapped for ours `{destructor, Execute, GetSize}` (the layout of every GTA IV DC, see
`CBaseDC`) and the payload is a `render::FrameSnapshot` (camera, clock, what to draw). Its
`Execute` runs on the render thread right after GTA's 3D scene and post-processing, before the
HUD, with the back buffer and GTA's depth buffer (INTZ, same size) still bound. There it drains
the render ring, uploads, draws the blocks (`render/World`) depth-tested against GTA's depth, and
composites Minecraft's overlay. Draws once per game frame (the second command of a frame only
drains); all state it touches is saved/restored with a per-draw `D3DSBT_ALL` state block.

- Camera: `TheCamera.m_pFinalCam` (rows right/forward/up/position, vertical FOV, near/far) as a
  camera-relative clip matrix (camera position subtracted in double). The render phase's
  grcViewport (FusionFix's `+0xB0`) holds no valid viewport in 1.0.8.0 and
  `grcViewport::sm_pCurrent` is a shadow-cascade viewport at that point (both logged); a one-time
  probe looks for the viewport in the phase object. `RenderCamera=` pins a source.
- Depth: with FusionFix loaded the depth buffer is logarithmic (FusionFix's z-fighting fix:
  `z/w = log2(w/near) / log2(far/near)`), which the vertex shader reproduces per vertex; otherwise
  standard D3D depth. `RenderDepth=` overrides.
- Resources: everything static is `D3DPOOL_MANAGED` (atlas with CPU-built mips, section vertex
  buffers, entity textures, the overlay texture) and per-frame geometry goes through
  `DrawPrimitiveUP`, so the game's device `Reset` needs nothing from us. Section meshes are stored
  as quads (4 vertices + a shared index buffer) when Minecraft's triangles come in (0 1 2)(0 2 3).
- Shaders: HLSL string literals (`render/Shaders.h`) compiled at startup with `D3DCompile` from
  `d3dcompiler_47.dll` (Proton's builtin, vkd3d-shader) to vs_3_0 / ps_3_0.
- Lighting (`render/Lighting.h`, `RenderLighting=gta`): the blocks are lit the way GTA IV lights
  its own world, from the values GTA used this frame, so they follow the time of day, the weather
  and interiors. Every GTA shader shares its globals at fixed registers (read from the game's
  `.fxc` parameter tables): the deferred sun pass lights `albedo * (gDirectionalColour.rgb * .w *
  N.L + lerp(Amb0, Amb1, how much the face looks down))` (`c17`, `c18`, `c37`, `c38`; `c38` holds
  Amb1 minus Amb0), the post-processing fog pass blends towards `globalFogColorN/Color` by view
  depth (`c41` to `c43`, plus the far desaturation `gDepthFxParams` `c16`), and the tone mapping
  multiplies by `Exposure * ToneMapParams.y / adapted luminance` (a 1x1 R32F render target), adds
  bloom above `ToneMapParams.x` times `ToneMapParams.z / 4`, then saturation, `ColorShift`,
  `ColorCorrect * 2` and a luminance gamma (`deSatContrastGamma`). Our draw command runs right
  after the tone mapping: the fog constants and the adapted luminance texture (sampler 5) are
  still bound and are used as they are; the sun/ambient and tone mapping registers have been
  reused by later passes by then, so the plugin watches `SetPixelShaderConstantF` (device vtable
  slot 109) and keeps, per frame, the sun/ambient set most draws used and the tone mapping
  constants its pass set (found by `TexelSize` = 1 / the back buffer size; three register layouts,
  by variant). The shader then does the same maths: HDR light with Minecraft's sky light as
  occlusion (less ambient, no sun under a Minecraft roof), GTA's fog and far desaturation by view
  depth, GTA's tone mapping (its bloom with the pixel's own light standing in for the blurred
  surroundings), `RenderExposure`; block light (torches, glowstone) is a warm floor on the screen,
  rain (`CWeather::Rain`) darkens faces that look up a little. Missing pieces fall back
  separately: no sun pass seen for half a second (or the hook couldn't be placed): Minecraft's own
  lighting (below); no tone mapping constants or no adapted luminance texture: an auto exposure
  stand-in (`RenderSaturation`, `RenderExposureKey`, `RenderExposureFloor`). The `GTA lighting`
  log line says which inputs are in use.
- Minecraft's own lighting (`RenderLighting=minecraft`, and the fallback): texture x vertex colour
  (tint, AO) x Minecraft's face shade (from the normal index) x the light-map curve of max(block,
  sky x day factor from GTA's clock), Minecraft gamma 0.5, `RenderExposure`.
- Cutout is alpha-tested; translucent is sorted by section, back to front.
- In a vehicle Minecraft's rider and its mount reach us a frame or two after GTA moved the seat
  (0.8 m at 50 m/s): the avatar is drawn at this frame's seat (the ped's matrix minus
  `VehicleSeatDrop`, read in `drawingEvent`) and the scene triangles within 2.5 blocks of Minecraft's
  feet (the mount) move by the same amount (`SplitMount`).

## Transport

Under Wine/Proton the plugin opens `Z:\dev\shm\libertycraft-bridge` (= `/dev/shm/libertycraft-bridge`)
with `CreateFileW(OPEN_ALWAYS)`, only ever grows it to `kMappingBytes`, and maps it with
`CreateFileMappingW(PAGE_READWRITE)` in three views (control+rings 32 MiB, overlay 95 MiB
optional, render ring 64 MiB). Without `Z:\dev\shm` it uses the named section
`Local\LibertyCraft_v1`. On attach it resets the host-owned regions, clears `mcPid`, then
publishes `magic`/`version`/`skyrimPid` (the host pid).

## Conventions

- Coordinates: `mc = (x, z, -y)`, `gta = (x, -z, y)`, 1 block = 1 m (`Coords.h`).
- Heading: GTA heading `h` (0 = north, counter-clockwise) and Minecraft yaw (0 = south,
  clockwise): `h = 180 - yaw`. One place: `kHeadingSign` / `kHeadingOffset` in `Coords.h`.
- Pitch: GTA camera pitch (up positive) = -Minecraft pitch (down positive).
- FOV: `McState.fovDeg` is vertical; `FovMode=vertical` copies it into `CCam::m_fFOV` (RAGE's
  grcViewport FOV is vertical; GTA IV is Hor+). `FovMode=horizontal43` converts to the
  horizontal FOV of a 4:3 view instead, if it turns out RAGE wants that.
- Camera matrix: the camera's right / forward / up are `CMatrix` rows `right` / `up` / `at`
  (0, 1, 2), pinned by default (`CameraRows=auto`) so the first puppet frames never use rows
  that are still being discovered. The game's own on-foot camera still cross-checks them once
  (logged as `camera rows check: ... agrees with the pinned rows`); `CameraRows=discover`
  adopts what it finds instead, `CameraRows=0,1,2` style pins other rows.
- Feet: `GET_CHAR_COORDINATES` is the ped's root (~1 m above the soles). The plugin measures
  root z - ground z while the player stands still (logged as `root->feet measured`) and uses
  `RootToFeet` (1.0) until then; puppeting sets the root to MC feet + that offset every frame.
- Placing the ped: not with `SET_CHAR_COORDINATES*`. Every one of those natives ends in
  `CTheScripts::ClearSpaceForMissionEntity` (1.0.8.0: `0x8B1390`), which deletes each ambient car
  and ped touching the player at the destination: every car or pedestrian the puppeted player
  walked into vanished. `PuppetMove=direct` makes only the natives' own move (a virtual call,
  vtable `+0x7C`, found in their common body at `0x8B2BF0`; the code bytes are checked at startup,
  otherwise the native is used).

## LibertyCraft.ini

Next to the `.asi` (`<gamedir>/plugins/LibertyCraft.ini`), written with defaults when missing.

| key | default | meaning |
|---|---|---|
| `Puppet` | `1` | Minecraft drives the player and camera (0: only watch and log) |
| `CameraMode` | `final` | `final`: overwrite `CCamera::m_pFinalCam` after the game's camera update; `scripted`: a `CREATE_CAM(14)` camera driven by natives |
| `FovMode` | `vertical` | `vertical` / `horizontal43`, see Conventions |
| `MenuKey` | `O` | opens Minecraft's pause/options menu (Esc is GTA's); a letter, digit or F1-F12 |
| `Diagnostics` | `0` | stats every second instead of every 10 s, camera matrix dumps |
| `LogPerf` | `1` | one frame-time line a minute (10 s with Diagnostics) |
| `FreezePed` | `1` | `FREEZE_CHAR_POSITION` while puppeting (0: zero the velocity every frame) |
| `RootToFeet` | `1.0` | metres from the ped root to its feet until measured |
| `MeasureRootToFeet` | `1` | measure it in game |
| `ProbeFrom` | `top` | unused since collision v2 (was the v1 heightfield probe start) |
| `ProbeHeight` | `3.0` | unused since collision v2 |
| `CameraRows` | `auto` | `auto` (0,1,2, cross-checked against the game's camera), `discover`, or e.g. `0,1,2` / `-0,1,2` (right, forward, up as CMatrix rows; `-` flips) |
| `Combat` | `1` | combat bridge (0: Minecraft's combat events are drained and ignored, the puppeted player stays invincible) |
| `PedDamageScale` | `10` | Minecraft damage x this = GTA health off a ped (ambient peds have 100) |
| `PlayerDamageScale` | `10` | GTA damage to the puppeted player / this = Minecraft damage |
| `ExplosionType` | `0` | `ADD_EXPLOSION` type for Minecraft explosions |
| `ExplosionRadiusScale` | `1.0` | Minecraft blast radius (blocks) x this = GTA radius (m) |
| `RagdollOnHit` | `1` | a Minecraft hit ragdolls the ped and pushes it along the knockback (away from the attacker) |
| `VehicleDamageScale` | `15` | Minecraft damage x this = GTA body and engine health off a vehicle (1000 each). An engine run below 0 catches fire and blows up a few seconds later; a hit on a burning one blows it up at once |
| `NpcBlocks` | `1` | Minecraft's blocks are solid for GTA's peds (pushed back out, they follow the wall) and cars and bikes (their speed into the blocks is taken off) |
| `NpcPushMethod` | `0` | not in the default ini: how a ped is moved out of blocks (`0` the entity's own set-position, `1` `SET_CHAR_COORDINATES_NO_OFFSET`) |
| `DebugKnockbackVariant` | `-1` | test hook: Minecraft's hits shove peds a different way each hit (0 world direction, 1 turned into the ped's frame, 2 the old flags, 3 no force), cycling from this one, and log how far along the push each went |
| `Render` | `1` | draw Minecraft's blocks and HUD in GTA's frame (0: only drain the render ring) |
| `RenderCamera` | `auto` | the blocks' camera: `auto` (the render phase's grcViewport, else the final cam), `phase`, `current` (grcViewport::sm_pCurrent), `finalcam` |
| `RenderDepth` | `auto` | GTA's depth buffer: `auto` (logarithmic when FusionFix is loaded, else standard), `log`, `standard`, `off` (blocks not hidden by GTA's world) |
| `RenderExposure` | `1.0` | brightness multiplier for the blocks (after GTA's tone mapping) |
| `RenderLighting` | `gta` | `gta`: GTA IV's sun, ambient, fog and tone mapping (see Rendering); `minecraft`: Minecraft's own light levels |
| `RenderSaturation` | `0.8` | colour saturation of the blocks when GTA's tone mapping constants are unavailable (else GTA's own) |
| `RenderExposureKey`, `RenderExposureFloor` | `0.85`, `11` | calibration of the auto exposure stand-in (not in the default ini) |
| `DebugTimeOfDay`, `DebugWeather`, `DebugStepSeconds` | | test hooks: pin GTA's clock to each hour of a list in turn (e.g. `12,19.5,21.5,0`) and force each weather of a list (`-1` leaves it; 0 extrasunny, 3 cloudy, 4 rain, 6 foggy), one step every `DebugStepSeconds` (20) once the blocks are drawn; logs `debug step i/n` |
| `DebugLightingAB`, `DebugLighting` | `0` | test hooks: alternate Minecraft's and GTA's lighting within each step; log GTA's lighting registers and the sun/ambient sets per step |
| `Overlay` | `auto` | Minecraft's HUD: `auto` (while puppeting or a Minecraft screen is open), `always` (whenever Minecraft is alive), `off` |
| `VehicleKey` | `F` | while Minecraft drives: GTA enters/steals the nearest vehicle (see Vehicles and Niko mode) |
| `ToggleKey` | `Backslash` | Minecraft mode <-> Niko mode. Key names: a letter, digit, F1-F12, `Backslash`, `Grave`, `Tab`, `Minus`, `Equals`, `LBracket`, `RBracket`, `Semicolon`, `Apostrophe`, `Comma`, `Period`, `Slash`, `Space`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Numpad0`-`9`, ... or a DIK code like `0x2B` |
| `HideNikoInVehicle` | `1` | hide Niko in vehicles in Minecraft mode (Minecraft's player rides its mount there) |
| `ToggleStartsInMinecraft` | `1` | start in Minecraft mode (0: Niko mode) |
| `VehicleSeatDrop` | `0.75` | metres from the seated ped's position down to the riding Minecraft player's feet |
| `VehicleEnterFallback` | `warp` | GTA's enter press didn't take within 2 s: `warp` (`WARP_CHAR_INTO_CAR`, closest car within 10 m), `task` (`TASK_ENTER_CAR_AS_DRIVER`), `none` |
| `DebugAutoToggle` | `0` | test hook (not in the default ini): toggle the mode every 10 s |
| `DebugAutoVehicle` | `0` | test hook: press the vehicle key when a car is within 12 m (else park an empty test car next to Niko first; indoors, move him to the nearest road), and GTA's exit control after 12 s in a car |
| `DebugVehicleDriver` | `0` | test hook: DebugAutoVehicle's test car gets a random driver (the press carjacks) |
| `DebugInjectEnterKey` | `0` | test hook: the vehicle key's taps as real key events (`SendInput`) instead of pad writes |
| `PuppetMove` | `direct` | how the puppeted ped is placed: `direct` (the natives' own move without their clearing, see Conventions) or `native` (`SET_CHAR_COORDINATES_NO_OFFSET`: deletes the cars and peds the player walks into) |
| `DebugWalkThroughCar` | `0` | test hook: walks the puppet target through a parked (ambient) car and into a pedestrian, once with each move method, and logs whether they survive |
| `DebugFocusCycle` | `0` | test hook: a window of the plugin's own takes the foreground for 3 s and gives it back, twice (alt-tab without a keyboard); SendInput mouse moves before and after show whether raw mouse input still arrives. The first cycle runs without the raw mouse watchdog |

## Vehicles and Niko mode

GTA IV takes the player back from Minecraft (`kSkyHostDrives`: Minecraft follows `SkyState`
pos/yaw with no physics, input or damage) when:

- **Niko mode** (`ToggleKey`, Backslash): plain GTA IV. No puppet, no input for Minecraft, GTA's
  HUD and mouse; Minecraft's player follows Niko. The toggle key works in both modes (it is
  never forwarded) and shows a short on-screen note. Back in Minecraft mode the teleport
  handshake runs at Niko's spot, then puppet mode.
- **`VehicleKey`** (F) while puppeting (never forwarded to Minecraft): puppet mode lets go and
  `processPadEvent` taps GTA's own `INPUT_ENTER`, so the game picks the door or carjacks as
  usual. GTA's enter check (1.0.8.0: `0xA60D0B`) only takes the press while the ped counts as
  standing (CPed flag word `0x26C` bit 0, set by the ped's ground probe), and every puppet frame's
  move clears that flag: after puppet mode lets go Niko stands again 0.1 to 0.5 s later. The old
  single press came before that and was lost, so the player's second F (GTA's own) did it. The
  taps (0.1 s each, up to 3, 0.45 s apart) start once GTA reads the player's pad again and Niko
  stands (at most 0.75 s), and stop as soon as Niko walks off to a door or is getting in (then
  no fallback while he walks). Releasing the ped also sets it down onto the ground when it hangs
  up to 0.3 m above its resting height (Minecraft's feet often sit a few cm higher than GTA's
  physics rests Niko): a ped that drops even a few cm is "landing" for over a second, and GTA
  ignores F meanwhile. A synthetic tap behaves like a real F (`DebugInjectEnterKey`): where
  GTA itself won't enter (some spots next to a car), the fallback does. No "getting in" after 2 s:
  `VehicleEnterFallback`. No vehicle after 4 s: back to Minecraft.
- **in a vehicle** (`IS_CHAR_IN_ANY_CAR`, however Niko got there: the key, a mission script, a
  cutscene): GTA drives, its own F gets out. `SkyState` also carries `kSkyInVehicle`, pos = the
  riding player's feet (ped position - `VehicleSeatDrop`), yaw = the vehicle heading; Minecraft
  puts its player on a mount there (a boat by default). Niko is hidden (`HideNikoInVehicle`).
  On foot again for 0.5 s: the teleport handshake, then puppet mode.
- getting into a vehicle (`IS_CHAR_GETTING_IN_TO_A_CAR`, e.g. a mission script) and cutscenes.

`Game::State()` exposes `hostDrives`, `inVehicle` and `nikoMode` for the renderer and overlay.
The decisions are `DriveLogic.h` (pure, tested by `asi/tests/drive_test.cpp`).

## Input while puppeting

Keys go to Minecraft as SDL3 scancodes (hardware scancode -> DirectInput code -> SDL). Esc and
`` ` `` stay GTA's (pause menu) unless a Minecraft screen is open; F1-F12 go to both. `MenuKey`
sends `kInOpenMenu`. Mouse: raw input (`WM_INPUT`) for look / the GUI cursor, buttons and wheel
from window messages. GTA reads DirectInput itself, but with player control off (puppet mode)
`CPad::GetPad()` hands the player's ped an empty pad, so its controls do nothing; `processPadEvent`
zeroes every CPad control except `INPUT_FRONTEND_PAUSE` for any frame where control is on anyway.
Focus loss and GTA menus send `kInReleaseAll`.

Raw input allows one mouse registration per process, and Wine's DirectInput registers it for its
own window (`RIDEV_CAPTUREMOUSE | RIDEV_NOLEGACY`) whenever GTA acquires its mouse and removes it
when GTA lets go: alt-tab does both. Afterwards no `WM_INPUT` (no mouse look) and, with
`NOLEGACY`, no mouse button messages reached the game window until puppet mode restarted (a trip
through GTA's pause menu did that: the "Esc twice" workaround). While puppeting and focused,
`Input::Tick` checks the registration every 0.1 s (every frame for 3 s after a focus change) and
takes it back (`raw mouse: the registration was taken over ...`, `retaken` in the stats).

## Log

`<gamedir>/LibertyCraft.log` (truncated each start, flushed per line, also `OutputDebugStringA`).
Lines look like `[HH:MM:SS.mmm] [module] text`. A healthy run shows, in order:

    [main] LibertyCraft 0.1.0 (...) loading; log ...
    [main] GTAIV.exe version 1.0.8.0: supported
    [config] config: Puppet=1 CameraMode=final ...
    [main] game version 1.0.8.0 detected (base 0x...); hooks placed: ...
    [link] link created (#1): file Z:\dev\shm\libertycraft-bridge, 200327168 bytes (191 MiB), ...
    [input] game window 0x... subclassed (focused 1); menu key dik 0x18
    [game] first SkyState written: flags 0x1, MC pos ..., viewport 1920x1080, hour ...
    [game] teleport #N: Minecraft to MC x y z (GTA ...), yaw ...
    [game] camera rows check: the game's on-foot camera agrees with the pinned rows (...)
    [game] root->feet measured: 1.0xx m ...
    [game] Minecraft connected: pid ..., heartbeat N ms old, ...
    [collision] kColClear epoch 1 sent
    [game] Minecraft acknowledged teleport #N (now at MC ...)
    [input] raw mouse registered for the game window (...)
    [game] puppet ON: Minecraft drives the player (...)
    [game] stats 10s: frames 60.0/s, SkyState writes 60.0/s (teleport #N, acked #N), ...
    [game] stats 10s: input keys ... raw-mouse ... pad-zeroed ...
    [game] stats 10s: collision regions ... (tris ..., blocks ...), columns probed ...
    [game] stats 10s: motion ... frames, ... MC ticks, ... late frames, render delay ... ms
    [render] render: CRenderPhase::sm_pCurrent @..., grcViewport::sm_pCurrent @..., FusionFix loaded (logarithmic depth)
    [render] first draw command executed: render thread ... (game thread ...)
    [render] block renderer ready (vs_3_0/ps_3_0, managed buffers)
    [render] blocks use the final cam camera (RenderCamera=auto)
    [render] received Minecraft's block atlas 2048x2576 (5 mip levels, ... ms)
    [overlay] overlay texture 1920x1080
    [render] render ring ... KiB in 10.0s: section ... atlasRegion ...; overlay frames ... (uploaded ...)
    [render] render: ... frames drawn ..., N sections (... MiB, atlas yes ...); per frame: drawn ... sections, ... draw ... ms
    [blocks] Minecraft's solid blocks arrived (N sections so far): peds and vehicles collide with them
    [blocks] stats 10s: N solid sections (...); peds pushed out N times (...), vehicles touching blocks N frames, slowed N ...
    [combat] stats 10s: vehicles N per table write; vehicle hits N (...), occupants hit N, windows N, set on fire or blown up N

`ERROR` / `WARNING` mark problems; an unsupported exe is a banner of `ERROR` lines.
