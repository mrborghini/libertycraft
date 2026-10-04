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
| `Game.*` | per frame: SkyState, McState, teleport handshake, mouse look, puppet mode, camera. Walking into or out of an interior (`SkyState.worldId`: 0 outdoors, else GTA's interior; same coordinates) keeps puppet mode: no teleport handshake and no new collision epoch, the 7 x 7 columns around the player are probed again and sent where they changed (`Collision::Refresh`), so Minecraft never loses the floor. Only the game moving the player (more than 5 m in a frame: a warp, a script, a respawn) runs the handshake, and meanwhile Niko stays hidden under the Minecraft body (HostDrive) |
| `Input.*` | window subclass, raw mouse, DIK -> SDL3 scancodes, CPad zeroing |
| `Collision.*`, `collision/*` | v2: GTA's static collision sampled with one-sided line probes (`CWorld::ProcessLineOfSight`) per 8x8-block column: floors, ceilings and walls -> kColTris / kColRegion (region scheduler from SkyCraft); the water grid. `collision/Geometry.h` is SDK-free and tested on Linux (`tests/collision_test.cpp`). Street furniture (lamp posts, bins, benches, hydrants: GTA *objects*) in the 5x5 columns around the player: each object probed once with OBJECTS-only probes that skip everything else, as oriented boxes merged into the column's regions; doors, tiny and attached objects are left out, moved objects are re-probed once still (`collision/Objects.h`, SDK-free, tested; logged as `collision objects 10s`) |
| `Render.*` | `drawingEvent`: snapshots the frame's camera (render phase grcViewport / final cam), clock and flags into a draw command of our own that GTA's render thread executes; drains the render ring there |
| `render/World.*` | the D3D9 block renderer: atlas + mips, section vertex buffers, entities/scene/avatar, outline and cracks, Minecraft lighting, depth-tested against GTA's (FusionFix log) depth |
| `render/RenderMath.h`, `render/Shaders.h`, `render/Frame.h`, `render/D3D9Util.h` | pure helpers (tested on Linux), the HLSL (compiled at runtime by d3dcompiler_47), the frame snapshot, D3D9 helpers |
| `render/Shadows.h`, `render/ShadowPass.*` | sun shadows (see Rendering): GTA's cascade layout and lookup (`Shadows.h`, SDK-free, tested in `tests/render_test.cpp`), our own cascade atlas, the caster passes and the pass that darkens GTA's world in the blocks' shadow |
| `Overlay.*` | Minecraft's GUI/HUD composited over the frame (premultiplied alpha, crosshair invert pass, cursor) |
| `HostDrive.*`, `DriveLogic.h`, `drive/VehicleHit.h` | vehicles and Niko mode: who drives the player (Minecraft or GTA IV); after a vehicle or a knockdown GTA keeps Niko until he really stands; any vehicle running into the puppeted player (or one of GTA's explosions) knocks them over, a helicopter's spinning rotor too (`RagdollOnVehicleHit`). `drive/VehicleHit.h` is SDK-free and tested on Linux (`tests/drive_test.cpp`) |
| `NikoBody.*`, `render/Body.h` | the Minecraft body on Niko's skeleton (`MinecraftBody`): while GTA animates Niko (vehicles, knockdowns, cutscenes, Niko mode) he is hidden and Minecraft's standing body (`kRenRagdoll`) is drawn with each part on his bones, sized to him (see Vehicles and Niko mode). `render/Body.h` is SDK-free and tested on Linux (`tests/body_test.cpp`) |
| `ViewportRoom.*` | the room GTA renders from at interiors: GTA IV draws an interior's rooms (and the outside only through their portals) from the room its game viewport's portal tracker is in, which follows the viewport's camera (Minecraft's, since we overwrite the final camera: measured) through the portals it crosses. Minecraft's third-person camera passes through the wall beside a door, so the tracker stayed in the stairwell with the camera out in the street and the building hung in a void. Each frame while puppeting (and 1.5 s after, while GTA's camera jumps back to Niko), near an interior (the camera in an interior's bounds, `GET_INTERIOR_AT_COORDS`, or the tracker in one, `GET_KEY_FOR_VIEWPORT_IN_ROOM` not -1): the camera outside every interior's bounds for 2 frames with the tracker in one: `CLEAR_ROOM_FOR_VIEWPORT` (outside); a camera away from the player's eye (third person) in an interior's bounds (they reach metres out into the street): the game's own room search where the camera is, each time it has moved 0.25 m (0.5 s at a slower pace): `SET_ROOM_FOR_VIEWPORT_BY_KEY` with key 0 (room boxes, then a probe down for the room the collision below belongs to; nothing: outside; 0x91E8C0 on 1.0.8.0), with this frame's camera position put into the viewport first (+0x80 of the object at 0x11F8290, code bytes checked). Logged as `[room] stats 10s` |
| `Coords.h` | GTA <-> MC coordinates, heading <-> yaw, camera basis |
| `Combat.*`, `combat/CombatMath.h` | actor table (peds, and vehicles as rows of small boxes -> Minecraft stand-ins: each car model's shape is measured once with line probes against the collision of a parked one (body width without the mirrors, ends, the top every 1/40 of the length, the greenhouse's width; logged as `vehicle shape`), and the boxes hug the body within 5 cm whichever way it faces, as tall as the bonnet and boot where they are, so a hit over the bonnet reaches whoever hides behind it, with a narrower row for the cabin), Minecraft hits/explosions/death -> GTA (vehicle hits: body/engine damage and fire; where a hit landed (`kEvHitPoint`) is followed into the car's model box, so a blow or an arrow on a side window's glass breaks that window (no native breaks a windscreen; the window native is checked against the game's code once, and only used on a settled, intact vehicle whose model has that window) and an arrow through any glass hits whoever sits behind it, who dies slumped in the seat), GTA damage to the puppeted player -> `kInHurt` (with the weapon's kind, found with `HAS_CHAR_BEEN_DAMAGED_BY_WEAPON` since the ped's last damage weapon field is unset on 1.0.8.0, and the direction it came from, so Minecraft's shield blocks what comes from in front; while the shield is up (`kMcBlocking`) such a hit leaves Niko quiet: GTA's pain voice (0x83AB60, hooked after a byte check) is skipped for hits from within the shield's front arc, the same test Minecraft makes, and reaction animations are off; hits from behind sound as usual); the puppeted player's health stays at a buffer of 1000 that GTA can't take, and GTA's radar arcs show Minecraft's health and armour instead (see `GtaHud`) |
| `Doors.*` | GTA's doors swing open for the Minecraft player (Niko is frozen with collision off, so he never pushes one): a door (object pool, `collision/Objects.h`'s door shape) opens away from the player when they walk into it, through GTA's door state (`SET_STATE_OF_CLOSEST_DOOR_OF_TYPE`; object heading or a push as fallbacks), and is shut and handed back to GTA once they are clear |
| `NpcBlocks.*`, `combat/BlockPush.h` | Minecraft's blocks are solid for GTA's peds and vehicles: `kRenSolids` bitsets (handed over by `render/World.cpp`'s ring consumer) push peds out of block columns (they follow walls round) and take the speed into blocks off cars and bikes; and they stop GTA's gunfire: GTA's bullet trace (0x92DAA0, hooked after a byte check) first marches each shot through the solid blocks (`blocks::RayFirstSolid`) and ends it at the first block's face, so nothing behind the blocks is hit (the player, peds, vehicles) and the tracer stops there; where nothing in front took the shot, GTA's concrete impact effect is triggered there and Minecraft is told (`kInBulletImpact`) to show the block's hit particles and sound (GTA's own effects are drawn under Minecraft's blocks). `BlockPush.h` is SDK-free and tested on Linux (`tests/combat_test.cpp`) |
| `Hazards.*`, `hazard/HazardGrid.h` | Minecraft's fire, lava, magma and water for GTA's peds and vehicles. The hazard bits of `kRenLights` (every fire, lava and magma block emits light) and `kRenLiquids` (each section's water and lava: surface height and kind) are handed over by `render/World.cpp`'s ring consumer and applied on the game thread. Peds on foot in fire or lava are set alight (`START_CHAR_FIRE`) and hurt as Minecraft hurts its own (magma only hurts), burn on for a few seconds and are put out; vehicles whose model box touches fire lose engine health until GTA's engine fire and explosion, lava sets them alight at once (`HazardsBurnVehicles`); in water or lava a vehicle struggles, more the deeper it sits (`LiquidsSlowVehicles`: only part of the engine's push takes, drag, a top speed it's eased down to, through its physics collider's velocity every frame), and over a metre of water its engine runs on for 3 s (GTA's own drowning measured 2.4 to 3.2 s), then stalls and is held off while it's that deep, and starts again out of it; water never damages a vehicle. Peds wade slower in it (`LiquidsSlowPeds`). `HazardGrid.h` is SDK-free and tested on Linux (`tests/hazard_test.cpp`) |
| `BlockyCity.*` | the blocky city (`kMcBlockyCity`, see The blocky city below): GTA's map geometry (buildings, terrain, props, their LODs and shadows) hidden while Minecraft's player is in it |
| `collision/Rays.h` (materials) | every line probe's hit carries GTA's material of the surface it hit (`HitMaterial`), and the floors, ceilings and walls built from the probes carry it in their `ColTri` flags (`kTriGtaMaterial`, bits 16 to 23) for the blocky city's blocks (`CityMaterials`) |
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
  `DrawPrimitiveUP`, so the game's device `Reset` needs nothing from us (but for the shadow atlas,
  see Shadows). Section meshes are stored
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
- Shadows (`render/Shadows.h`, `render/ShadowPass.*`, `RenderShadows=1`): the blocks take GTA IV's
  sun shadows and cast their own, onto themselves and onto GTA's world. FusionFix 5 renders GTA's
  sun shadow cascades into one R32F atlas (4 cascades side by side, width = 4 x height, ending at
  10, 30, 85 and 256 m) that is still bound on sampler 15 at our draw command. Its deferred sun pass
  (`deferred_lighting.fxc`) looks a point up with the shadow globals `c53` to `c63`
  (`gShadowMatrix`, each cascade's uv scale and offset, `gFacetCentre`: the camera's depth plus the
  cascades' ends) and FusionFix's filter settings (`c217` to `c223`: softness, normal offset bias,
  4 or 16 rotated Poisson taps, CHSS, cascade blend). The `SetPixelShaderConstantF` watch keeps the
  set most writes used among those made for this frame's camera, and the world shader repeats GTA's
  lookup and filter: a block's sun term is multiplied by it (no sun shadows when GTA's sun pass used
  none for half a second, indoors for example). The blocks, the entities, the avatar and the body
  are drawn into an atlas of our own with the same layout, cascade by cascade (scissored to the
  cascade's quarter; sections culled per cascade and beyond `RenderShadowDistance`), storing the
  value GTA compares with, so one lookup gives both GTA's shadow and GTA's plus the blocks', with
  GTA's own filtering. Before the blocks are drawn, a full-screen pass reads GTA's depth buffer (the
  INTZ texture behind it), rebuilds each pixel's position and normal and multiplies the frame by how
  much less sun the blocks leave it, as GTA's tone mapping shows it (`render/Shadows.h` DarkenFactor):
  the light `ambient * ao + sun * N.L * both` against `ambient * ao + sun * N.L * GTA's` (GTA's own
  split: `ao` is its G-buffer 2's z, still bound on s0 after its tone mapping, and N.L gets FusionFix's
  remap `saturate(N.L * 4/3 - 1/3)`, as in its sun pass), the shaded light pulled towards the lit one by
  GTA's fog, both through GTA's saturation step and luminance gamma (`deSatContrastGamma`), per colour
  channel. So what GTA already shades is left alone and a block shadow is as dark as GTA's on the same
  ground, with the tint GTA's own shadows have there (`RenderShadowStrength`): the HDR ratio alone is
  deep blue under the noon sky and pink under the cyan moon, which GTA's desaturation (0.34 to 0.75)
  mostly takes out of its picture. The
  blocks' own sun term uses the same N.L remap. Our atlas is `D3DPOOL_DEFAULT`: a hook on
  `IDirect3DDevice9::Reset` releases it first. The shadow constants sit in the shaders' low registers
  (`c14` to `c27`; placed at `c100` and up, DXVK sometimes left them unset for the block shader), and
  the `SetPixelShaderConstantF` watch ignores what our own draw command writes. The `GTA sun shadows`
  log line says what is in use; every 10 s a `shadows in N frames` line gives both passes' CPU and
  GPU times (about 0.25 ms CPU and 0.7 ms GPU at 500 sections, measured).
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
| `PuppetPlayerControl` | `1` | the player keeps GTA's player control while puppeted (its pad is zeroed, so GTA doesn't move him): without it GTA's peds and police take him for a cutscene player and stop fighting and chasing him |
| `GtaHud` | `1` | keep GTA IV's HUD and radar next to Minecraft's HUD in Minecraft mode; GTA still hides them itself in cutscenes and menus (0: Minecraft's HUD only). While Combat owns Niko's health the radar's ring mirrors Minecraft (its health, max health and armour reach us in `McState`'s padding, `kMcVitalsValid`): the HUD's three reads of the player's health (the arc, the flash of what was just lost, where the armour arc starts) call a stub that answers 100 + 100 x Minecraft's health fraction, and its two reads of his max health read 200 (0x87068E, 0x8706C0, 0x86B019, 0x86C869, 0x86C8A5 on 1.0.8.0, each checked byte for byte first), so GTA's own HUD code shows Minecraft's health as its own: the loss flash, the red blink at a quarter or less, the armour arc right after the health arc. GTA's real health stays at the 1000 buffer. GTA's armour is set to Minecraft's armour points x 5 (it absorbs GTA's damage first, which is still sent to Minecraft, and is put back each frame). In Niko mode and vehicles GTA's own values show |
| `PuppetCollision` | `0` | not in the default ini: the puppeted ped keeps its collision (not needed for GTA's peds and police to fight him: with player control on, their shots and blows reach him either way) |
| `RootToFeet` | `1.0` | metres from the ped root to its feet until measured |
| `MeasureRootToFeet` | `1` | measure it in game |
| `ProbeFrom` | `top` | unused since collision v2 (was the v1 heightfield probe start) |
| `ProbeHeight` | `3.0` | unused since collision v2 |
| `CameraRows` | `auto` | `auto` (0,1,2, cross-checked against the game's camera), `discover`, or e.g. `0,1,2` / `-0,1,2` (right, forward, up as CMatrix rows; `-` flips) |
| `Combat` | `1` | combat bridge (0: Minecraft's combat events are drained and ignored, the puppeted player stays invincible) |
| `PedDamageScale` | `10` | Minecraft damage x this = GTA health off a ped (ambient peds have 100) |
| `PlayerDamageScale` | `10` | GTA damage to the puppeted player / this = Minecraft damage |
| `ExplosionType` | `0` | `ADD_EXPLOSION` type for Minecraft explosions |
| `ExplosionRadiusScale` | `1.0` | Minecraft blast radius (blocks) x this = GTA radius (m). (`ADD_EXPLOSION`'s size argument is a share of the type's own radius from `explosionFx.dat`, which the game clamps to 0.01 to 1 (seen in its code, native 0xB13340 -> 0x9940D0): TNT and creepers make their type's full blast; firework blasts pass the share) |
| `FireworkExplosionType` | `2` | `ADD_EXPLOSION` type for a firework rocket with stars (`kExplosionFirework`): the crossbow is an RPG. The burst (Minecraft's `FireworkRocketEntity.explode`: end of flight, or where it struck a creature, a stand-in or a surface; crossbow, hand launch, dispenser or elytra boost) becomes GTA's rocket blast there, 4 m across for one star, a metre more per star, the rocket's full 8 m from five, at full RPG damage within it (peds die and fly, cars burn); no stars, no burst (vanilla). Minecraft's own firework damage never reaches the stand-ins, so peds and vehicles are hurt once. A rocket that struck a vehicle's stand-in also does the rest of an RPG's hit to it a quarter second later if it still runs (500 body and engine health a star, `CombatMath.h FireworkHitDamage`; an engine run below 0 blows it up at once). In game one one-star crossbow rocket brought a hovering police Maverick down every time, 43 m and 80 m away: GTA's blast took its body to 0 and killed the pilot (wrecking it outright twice in three), the rocket's own hit blew up the rest, and it fell; two stars wreck anything with 1000 engine health. Helicopters are stand-ins out to a crossbow rocket's reach (`kAircraftRange`: 1.6 blocks a tick for at most 52 ticks at flight duration 3 = 83.2 m, plus 10 m), ground vehicles to 60 m. The player's own rockets are `CAUSE_EXPLOSION` as far as they fly |
| `RagdollOnHit` | `1` | a Minecraft hit ragdolls the ped and pushes it along the knockback (away from the attacker), about as far as a gunshot moves a body: a plain hit 1 to 1.5 m, the hardest Knockback hit about 4 (`CombatMath.h HitShoveForce`; the old shove sent them 7 to 9 m). A killing blow on a ped on foot ragdolls and pushes it first and deals the killing damage a couple of frames later, so it dies limp and flies like a gunshot victim instead of playing GTA's death clip (a ped that refuses the ragdoll, about one in ten, still plays it); vehicle occupants keep their seated death. Corpses stay in the actor table (`kActorDead`, a low box over the body between the pelvis and the neck, not solid): a hit or an arrow pushes a body still in its death ragdoll along the knockback (a force, harder for more damage and knockback, a couple of metres at most: QA saw a 30-damage blow send one 25 m); a settled one lies animated in its dead pose and GTA's dead task won't let it ragdoll again (`SWITCH_PED_TO_RAGDOLL` holds for one frame), so it is thrown along a short arc as it lies (`CorpseFlings`). Peds aren't solid for the Minecraft player: running into one sends `kEvBump` (the player's speed, from its motion this tick, so falls and elytra count) and the ped is moved out of the way at a walk, stumbles from a sprint (5.2 m/s: a 1.5 s balance ragdoll, `SWITCH_PED_TO_RAGDOLL` kind 2, and a light push, so it staggers and stays on its feet), is knocked down from 8 m/s and hurt from 10 m/s (8 health a m/s: an elytra pass at 23 m/s kills, dying in the ragdoll), as a crime (`HIT_PED`); a corpse walked into is dragged and rolled along (CombatMath.h `BumpOf`) |
| `VehicleDamageScale` | `15` | Minecraft damage x this = GTA body and engine health off a vehicle (1000 each). An engine run below 0 catches fire and blows up a few seconds later; a hit on a burning one blows it up at once |
| `NpcBlocks` | `1` | Minecraft's blocks are solid for GTA's peds (pushed back out, they follow the wall) and cars and bikes (their speed into the blocks is taken off), and they stop GTA's bullets (not its explosions) |
| `GtaCrimes` | `1` | Minecraft's attacks are GTA crimes: the victim fights back (cops, gangs, one civilian in three) or runs, a driver whose car is hit drives off, and the attack goes to GTA IV's own crime report (`CCrime::ReportCrime`, 0xA503E0, checked byte for byte first) with the player as the criminal: arrows are `SHOOT_PED`/`SHOOT_COP`, blades `STAB_`, anything else `HIT_`, a hit car `DAMAGE_TO_PROPERTY` (`HIT_COP` for a police car's driver), an explosion near the player (the player's firework rocket wherever it flies) `CAUSE_EXPLOSION`; GTA's wanted system then does the rest (witnesses, points, escalation up to the game's maximum, decay). Police killed in one wanted episode also set a floor (1: 2 stars, 3: 3, 6: 4, 10: 5, 15: 6, within `GET_MAX_WANTED_LEVEL`). If the report function isn't found, LibertyCraft's own rules give the wanted level |
| `DebugWanted` | `0` | test hook: N s into play, 2 wanted stars; then every 3 s for a minute the police's interest (wanted level, cops near, in combat, the shots they fired in those 3 s counted every frame, as `IS_CHAR_SHOOTING` only holds for the frame of a shot, and how often their gunfire hurt the player) and the player's health |
| `DebugStumbleKind` | `-1` | test hook: the ragdoll kind a stumble uses (`SWITCH_PED_TO_RAGDOLL`'s fourth argument, seen in the game's code: 0 a limp fall, 1 scripted, 2 a balance; -1 the default, a balance) |
| `NpcPushMethod` | `0` | not in the default ini: how a ped is moved out of blocks (`0` the entity's own set-position, `1` `SET_CHAR_COORDINATES_NO_OFFSET`) |
| `DebugTestCar` | `0` | test hook: N s into play, park an Admiral with a driver and a front passenger 4 m east of the player (left side toward the player, frozen), log its body health and its people's health as they change, and 90 s later move it 6 m in front of the camera for a screenshot. `-N`: park it 6 m in front of the camera with all four seats taken, and from 8 s on (well past the new-vehicle guard) shoot a lethal arrow through each side window in turn (the same code path as Minecraft's hits; 12 Minecraft damage, so the car stays whole for all four) |
| `DebugTestCarModel` | `admiral` | test hook: DebugTestCar's model (`sabre` has two doors, `pcj` is a motorbike) |
| `DebugCarCover` | `0` | test hook: DebugTestCar parks its car empty instead, facing north with its left side 0.9 m east of the player and the middle of its bonnet level with him, and a ped stands just beyond its right side there (for `tools/fake_minecraft.py --pick-test`: Minecraft's hits over the bonnet must reach him) |
| `DebugBulletWall` | `0` | test hook: N s into play, a ped with a pistol stands 1.5 m to the player's right and another 6 m ahead, and the first keeps shooting at the second (with a wall of blocks between them its shots stop at the wall: the `bullets` stats line, the second ped's health) |
| `DebugFireworkTargets` | `0` | test hook: N s into puppet mode, targets in the direction the player looks: a police Maverick (police pilot) hovering `DebugFireworkHeliAhead` (40) m ahead and 18 m up (a quarter of any extra distance higher, turned or lifted to where GTA's map leaves it room), side on, held there until something hurts it, and 14 m ahead two peds standing still left of a spot and a parked Admiral right of it; logs their spots in Minecraft's coordinates (`DebugFireworkTargets: aim`, for an autorun's `summon firework_rocket` with a `Motion` toward them) and their health, wrecks and ragdolls as they change for 60 s |
| `DebugBumpPed` | `0` | test hook: N s into play, a ped stands still 3 m ahead of the player (to run, fall or glide into: `tools/fake_minecraft.py --charge`, or an autorun's `tp` above it); its health, ragdoll and position are logged every second for 20 s |
| `DebugDieInCarAB` | `0` | test hook: killing blows on vehicle occupants cycle through the ways of dealing them (`DAMAGE_CHAR`, `EXPLODE_CHAR_HEAD`, `SET_CHAR_HEALTH 0`), and each killed occupant's pelvis is logged in the car's frame 0.5, 1.5 and 4 s later |
| `DebugKnockbackVariant` | `-1` | test hook: Minecraft's hits shove peds a different way each hit (0 world direction, 1 turned into the ped's frame, 2 the old flags, 3 no force), cycling from this one, and log how far along the push each went |
| `Render` | `1` | draw Minecraft's blocks and HUD in GTA's frame (0: only drain the render ring) |
| `RenderCamera` | `auto` | the blocks' camera: `auto` (the render phase's grcViewport, else the final cam), `phase`, `current` (grcViewport::sm_pCurrent), `finalcam` |
| `RenderDepth` | `auto` | GTA's depth buffer: `auto` (logarithmic when FusionFix is loaded, else standard), `log`, `standard`, `off` (blocks not hidden by GTA's world) |
| `RenderExposure` | `1.0` | brightness multiplier for the blocks (after GTA's tone mapping) |
| `RenderLighting` | `gta` | `gta`: GTA IV's sun, ambient, fog and tone mapping (see Rendering); `minecraft`: Minecraft's own light levels |
| `RenderSaturation` | `0.8` | colour saturation of the blocks when GTA's tone mapping constants are unavailable (else GTA's own) |
| `RenderShadows` | `1` | sun shadows: the blocks take GTA IV's and cast their own onto themselves and GTA's world (see Rendering) |
| `RenderShadowStrength` | `1.0` | how dark GTA's world gets in the blocks' shadow (1: as GTA shades its own; 0: the blocks shade only blocks) |
| `RenderShadowBias`, `RenderShadowDistance`, `RenderShadowCast` | `0.05`, `128`, `1` | not in the default ini: metres the blocks' casting surfaces move away from the sun (against acne), how far from the camera blocks cast (m), and `0`: the blocks only take GTA's shadows |
| `DebugShadows`, `DebugShadowsAB` | `0` | test hooks: log GTA's shadow constants (on changes and every 10 s) and write both atlases to `<gamedir>/libertycraft-shadow-gta.pgm` and `-ours.pgm` (15 s after the shadows come on, then every 30 s); `DebugShadowsAB=N` switches the shadows off and on every N s, logging `DebugShadowsAB: shadows on/off` |
| `DebugShadowView` | `0` | test hook: the blocks show their shadow term instead of their colour (red GTA's, green GTA's and the blocks', blue 0.5) |
| `DebugShadowSpot` | | test hook: `x,y,z,heading` (GTA), 12 s after the blocks are first drawn put the player there once (e.g. `1191.6,202.6,32.5,149`, the sunny sidewalk outside Schottler Medical Center) |
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
| `PhoneKeys` | `1` | GTA's phone in Minecraft mode: the arrow keys are GTA's, and Enter, Backspace and the number keys while the phone is out (see Input while puppeting) |
| `DebugPhone` | `0` | test hook: real key events that log which pad controls some keys feed, then take the phone out, open the phone book, scroll and put it away |
| `PuppetMove` | `direct` | how the puppeted ped is placed: `direct` (the natives' own move without their clearing, see Conventions) or `native` (`SET_CHAR_COORDINATES_NO_OFFSET`: deletes the cars and peds the player walks into) |
| `DebugWalkThroughCar` | `0` | test hook: walks the puppet target through a parked (ambient) car and into a pedestrian, once with each move method, and logs whether they survive |
| `DebugFocusCycle` | `0` | test hook: a window of the plugin's own takes the foreground for 3 s and gives it back, twice (alt-tab without a keyboard); SendInput mouse moves before and after show whether raw mouse input still arrives. The first cycle runs without the raw mouse watchdog |
| `RagdollOnVehicleHit` | `1` | any vehicle running into the puppeted player knocks them over: GTA takes Niko, ragdolls him along the vehicle's way and keeps him until he is back up; the hit hurts the Minecraft player (2.5 GTA damage per m/s / `PlayerDamageScale`, helicopters, planes and trains 1.5 times that, plus what GTA does to him while he tumbles). Cars, bikes, boats and trains are their model box; a helicopter's or plane's body is measured once per model with line probes against its collision (as slabs along its length: the cabin, the thin tail boom; not the rotor disc that its model box spans). What counts is the speed of the vehicle's point where the player is, from its pose this frame and last (its motion, its turning, up and down): 3 m/s or more (helicopters, planes, trains 2), into the player (not a vehicle pulling away from him), so a helicopter coming down on him or its tail swinging round hits like a bumper. A helicopter's spinning rotors (main and tail, hubs from the vehicle structure's bones, spin measured from the moving rotor bone) strike the player where his column crosses their disc: 180 GTA damage (18 Minecraft, tail 120) and a hard fling out from the hub (16 m/s, tail 12), and he falls invincible for 1 s (GTA's own rotor would chop him for 660 more). One of GTA's explosions hurting the puppeted player (20+ health) does the same, away from what blew up. The throw matches GTA's own run-over (`DebugVehicleHit=ped`: a pedestrian hit by a car at 10.1 m/s went off at 8.8 m/s and came to rest 10.9 m away): he is first moved out of the vehicle's box along the push (puppet mode leaves him frozen with collision off, so the vehicle is already into him when the hit is seen), clear of its next frames' travel (stopped short of the map), then ragdolled and pushed off at 0.87 of the vehicle's speed (1.15 times that for helicopters, planes and trains; at least 3.5 m/s). `APPLY_FORCE_TO_PED` sends him off at 1.8 m/s per unit of force (measured: 12 -> 21 m/s, 19 -> 34 m/s); every knockdown's push is capped at 20 m/s. (The old forces, 2 per m/s, threw him 40 to 90 m.) Where each knockdown took him is logged (`came to rest N m from where he was hit`) |
| `MinecraftBody` | `1` | while GTA animates Niko, show the Minecraft body following his animation instead of him (master switch) |
| `MinecraftBodyCutscenes`, `MinecraftBodyVehicles`, `MinecraftBodyNikoMode` | `1`, `1`, `0` | ... in cutscenes; getting into, driving, bailing out of and getting out of vehicles (0: Niko as before, the Minecraft player on its mount in the seat); in Niko mode. Knockdowns follow `MinecraftBody` alone |
| `MinecraftBodyScale` | `1.0` | the body's size on top of the automatic fit (torso and arms Niko's size, the top of the head at his, the feet on the ground); 0.5 to 2 |
| `MinecraftBodyHide` | `visible` | not in the default ini: how a ped under the body is hidden, `visible` (`SET_CHAR_VISIBLE`, every frame) or `alpha` (`SET_PED_ALPHA` 0) |
| `DebugBody` | `0` | test hook: log the skeleton once a second (bones, limb motion, ankle heights) and, in cutscenes, the animated objects around the camera |
| `DebugBailOut`, `DebugRunOver` | `0` | test hooks: DebugAutoVehicle's car drives off at 14 m/s and Niko bails out of it 2 s later; every 40 s of puppeting (outdoors; else moved to the road) a test car 15 m up the road drives at the player at 12 m/s |
| `DebugVehicleHit` | | test hook: a comma list of `heli` (a Maverick flies at the player at 8 m/s, a little off the ground), `drop` (one 9 m over him comes down at 4 m/s), `rotor` (one creeps at him at 3 m/s until its hub is 2.5 m away: stand him on something about 3 m up, its rotor reaches him first), `bike` (a PCJ at 12 m/s), `car` (an Admiral at 10 m/s; `car-old`: his knockdown dealt the old way, in place; `car-gta`: moved clear, then the car's own collision knocks him down), `ped` (GTA's own run-over to compare with: a pedestrian stands still 9 m ahead of the camera, 3 m to the right, and an Admiral comes at it from 15 m beyond at 10 m/s; where it comes to rest is logged), run one after another from 30 s into puppet mode, ahead of the camera; the cars drive on unpushed once they touch him; each vehicle is logged every 0.25 s (distance, velocity, rotor spin) and deleted 8 s on |
| `DebugCutscene` | | test hook: play this cutscene (e.g. `intro`, `rom2_a`; names from `pc/anim/cuts.img`) 20 s into puppet mode; its camera is logged every 2 s. What the game's own script sets up first isn't done: `intro`'s ship never appears (its people float over the sea; `LOAD_SCENE` at the cutscene's camera, 1.5 km from the player, didn't bring it), and `rom1_a` didn't load within 30 s |
| `DebugViewportRoom` | `0` | test hook: `1` logs the room GTA renders from against the camera's place every 0.25 s near interiors (`ViewportRoom`); `2` also switches the correction off and on every 8 s |
| `DebugGiveWeapon` | `0` | test hook: give Niko this GTA weapon (7 pistol) 10 s into play (puppet mode puts it away, Niko mode gives it back) |
| `HazardsBurnPeds` | `1` | Minecraft's fire and lava (soul fire, lit campfires too) set GTA's peds on foot alight and hurt them like Minecraft's own (2 and 8 Minecraft damage a second, magma 1, x `PedDamageScale`); out of it they burn on for 4 s (lava 8 s) and are put out. Niko too, unless Minecraft owns him (puppet mode, knocked over) |
| `HazardsBurnVehicles` | `1` | a vehicle on fire blocks loses 250 engine health a second (below 0: GTA's engine fire, and its explosion if it keeps burning), on magma 50; in lava it's set alight and wrecked in seconds. Traffic, parked cars and the player's own; their real model box |
| `LiquidsSlowVehicles` | `1` | vehicles struggle in Minecraft water and lava, the deeper the worse (about 8 m/s top in 0.2 m of water, 3 in 0.5, a crawl in 0.9); over a metre of water the engine runs on for 3 s, then stalls (held off while that deep, starts again out of it). Never damage from water |
| `LiquidsSlowPeds` | `1` | GTA's peds wade slower in Minecraft water (1 at ankle depth down to 0.35 of their speed) |
| `MinecraftWaterIsGtaWater` | `0` | not in the default ini, experimental: GTA's water level query (0x9AB6C0 on 1.0.8.0, hooked after a byte check) answers Minecraft's water surface where there is Minecraft water (not for scripts' `GET_WATER_HEIGHT*`, nor around the puppeted player). Measured: its callers are the camera, the player's position and a few effects; GTA's ped swimming and vehicle buoyancy don't use it |
| `DebugHazards` | `0` | test hook: to the street 15 s into puppet mode; every 15 s the nearest walking ped is put into Minecraft water near the player (its speed logged); every vehicle in Minecraft's blocks is logged (not just the player's); the player's stalled car is pulled back out of the water (twice), to see its engine start again; with the water hook, who asks it |
| `DebugHazardInject` | | test hook: `fire` / `lava`: a 3 x 3 patch of it (in the plugin only) under the nearest walking ped every 20 s; `harbour` / `shore`: 30 s into puppet mode a test car with Niko in it is dropped into GTA's own water (or set on the land before it, to drive in with `DebugDriveThrottle`) and GTA's own drowning is timed |
| `DebugDriveThrottle` | `0` | test hook: in a car, hold GTA's accelerator this many seconds (from 1 s in), the speed logged |
| `CityMaterials` | `1` | not in the default ini: collision triangles carry GTA's material of their surface (`kTriGtaMaterial`), so the blocky city builds tarmac, pavement, brick, glass, grass, ... out of the right blocks (0: Minecraft guesses from the kind of surface) |
| `DebugMaterials` | `0` | test hook: logs the line probes' unnamed result fields (histograms every 20000 hits, and some downward hits with where they hit): how the material was found |

## The blocky city (kMcBlockyCity)

A nether portal lit in the mirror world leads Minecraft's player into `libertycraft:blocky_city`, a
block copy of Liberty City at the same coordinates (1:1) that the mod builds from the collision this
plugin streams (it keeps every region it receives, see the root README). Everything stays in GTA IV:
Minecraft stays hidden and linked, puppet mode, teleports, combat and vehicles go on, and the city's
blocks arrive through the render ring like any others (a level change makes the mod send
`kRenClearAll` and the new dimension's sections, so nothing of the mirror world stays drawn).
McState carries `kMcBlockyCity`, and `BlockyCity.*` hides GTA's own map geometry meanwhile, so the
blocks are the city around Niko:

- every frame, every building in the building pool (map sections, terrain, interiors and their
  LODs) and every loose object in the object pool that belongs to the map (street furniture, props,
  doors; not weapons, pickups, vehicle or ped parts, nor anything attached to a ped or a car) gets
  its visible flag (`CEntity` `+0x24` bit 5) and its cast-shadows flag (`+0x28` `0x80`) cleared, so
  what streams in while the player walks around disappears too; the sky, the water surface, peds,
  vehicles and the HUD stay. The scan is logged every 10 s with its cost;
- back in the mirror world (the bit clears) every entity hidden that is still there with the same
  model gets both flags back; a game load forgets the list;
- GTA's collision isn't touched: it is the same city, so its peds and cars walk and drive on it. The
  city's own blocks therefore don't come in `kRenSolids` / `kRenLiquids` (the mod compares each
  block with what the city's generator put there), so `NpcBlocks` and `Hazards` see only what the
  player built in the city.

The blocky city's blocks come from the triangles' materials (`kTriGtaMaterial`). The material of a
line probe's hit is the low byte of the `tLineOfSightResults` dword at `+0x48`, its index in
`common/data/materials/materials.dat` (0 `DEFAULT` ... 155): measured with `DebugMaterials` on
1.0.8.0, the street gave 8 `TARMAC`, the pavement 3 `CONCRETE` and 16 `PAVING_SLABS`, a roof 54
`LEAD_ROOFING`, Roman's flat 87 `CARPET`, 86 `LINOLEUM` and 20 `WOOD_BOARD`. The bytes above it hold
other things (`0x0001xxxx`, `0x0060xxxx`, `0x02xxxxxx` seen); `+0x4C` looks like the polygon (or
component) index, `+0x50` was always 1. Floors and ceilings take the material most of their corner
samples were probed on, walls their probe's hit; walls found only from the far sample's solid and
street furniture (object probes) carry none.

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
  puts its player on a mount there (a boat by default). Niko is hidden (`HideNikoInVehicle`), or
  the Minecraft body sits in his place (`MinecraftBodyVehicles`, below). On foot again and standing
  (getting back up, below): the teleport handshake, then puppet mode.
- getting into a vehicle (`IS_CHAR_GETTING_IN_TO_A_CAR`, e.g. a mission script) and cutscenes.
- **getting back up**: out of a vehicle (a bail-out from a moving car rolls and tumbles), knocked
  over (`RagdollOnVehicleHit`) or ragdolled by GTA in Minecraft mode, GTA keeps Niko until he
  really stands: on his feet (`IS_PED_RAGDOLL`, `IS_CHAR_GETTING_UP`, `IS_CHAR_IN_AIR` all false,
  `GET_CHAR_SPEED` under 0.5 m/s, GTA's own standing flag) for 0.4 s, at least 0.5 s on foot, at
  most 8 s. A knockdown is applied once puppet mode has let go of him (`SWITCH_PED_TO_RAGDOLL`,
  then `APPLY_FORCE_TO_PED` in world axes, as Combat measured; a vehicle's moves him out of its box
  first, see `RagdollOnVehicleHit`); while knocked over Minecraft
  still owns his health (Combat's buffer), so GTA damage meanwhile goes to Minecraft and GTA can't
  kill him. While he gets back up (on foot) the player's input isn't GTA's (`padLocked`: his pad
  is cleared as in puppet mode, no aiming or firing his gun); only vehicles and Niko mode give GTA
  real player input. Handing back, the body stays on him (and he hidden) until puppet mode has
  him, so no frame shows Niko in between.

**The Minecraft body** (`MinecraftBody`, `NikoBody.*`, `render/Body.h`): while GTA drives for one
of the reasons above (per `MinecraftBodyCutscenes` / `Vehicles` / `NikoMode`), Niko is hidden
(`SET_CHAR_VISIBLE` every frame, also right after leaving puppet mode, which shows him) and the
standing body Minecraft sends (`kRenRagdoll`: parts tagged per model part, held items on their
arm; re-sent at once when the skin, armour or held items change) is drawn instead of the avatar,
each part posed by his bones as `drawingEvent` reads them (`GET_PED_BONE_POSITION`; offsets give
the head bone's axes): the body leans with hips -> neck and the shoulder line, legs point along
hip -> ankle, arms from Minecraft's shoulders at his hands (hands on the wheel), the head turns
with his head bone (x up the neck, y forward, z left; checked once by `CalibrateHead`), and the
figure moves so its lower sole meets his. Sizes: the torso and arms by his hips -> neck length
(about 0.81 of Minecraft's), the head so its top is his (about 0.65), the legs his length (about
1.3), times `MinecraftBodyScale`; so the head stays inside a car. Cutscenes: GTA IV's cutscene
Niko is no ped but an animated object of the player's model (object pool); its bones come from
the game's own bone matrix function (1.0.8.0: `0x941E30`, code bytes checked) after
`CDynamicEntity::GetBoneMatrix` waits for its pose job. The game only poses what it draws, so it
can't be made invisible (invisible, it stops moving; an alpha of 0 fades back in by 16 a frame, a
ghost over the body): its matrix is shrunk to 5% instead (a doll a few centimetres tall inside the
body, its bones scaled back up when read). Peds keep animating while invisible (measured).
Seated in a vehicle (`IS_CHAR_IN_ANY_CAR`) the held items are hidden (collapsed onto the hand:
`Pose::part[kPartNone]`, which no batch uses, places them before their arm does), as the sword
and shield poked through the roof. GTA's camera at the body (a helicopter coming down pushed it in
between the wall and the body, which is bulkier than Niko): with GTA's own camera (knocked over,
vehicles) within its near plane + 0.3 m of any part's box (with the outer skin layer and armour)
the whole body is hidden for that frame, as GTA fades its own player; a cutscene's camera (close-ups)
only hides the parts it is in or within its near plane + 0.1 m of (`camera at (or in) the Minecraft
body` in the log).

`Game::State()` exposes `hostDrives`, `inVehicle` and `nikoMode` for the renderer and overlay.
The decisions are `DriveLogic.h` (pure, tested by `asi/tests/drive_test.cpp`).

## Input while puppeting

Keys go to Minecraft as SDL3 scancodes (hardware scancode -> DirectInput code -> SDL). Esc and
`` ` `` stay GTA's (pause menu) unless a Minecraft screen is open; F1-F12 go to both. `MenuKey`
sends `kInOpenMenu`. Mouse: raw input (`WM_INPUT`) for look / the GUI cursor, buttons and wheel
from window messages. GTA reads DirectInput itself and the player keeps player control while
puppeted (`PuppetPlayerControl`), so `processPadEvent` clears every CPad control except
`INPUT_FRONTEND_PAUSE` and, with `PhoneKeys`, the phone's (below): buttons to 0, the axis controls
(move, look, mouse, frontend and vehicle axes, sniper zoom) to their rest value 128 (measured: 0 is
a full push, and with player control on Niko walked off while getting back up). The same while
Niko gets back up (above). Niko is hidden in every Minecraft camera mode (F5 too: Minecraft's body
is drawn there), every frame. GTA's weapon is put away while puppeting (its HUD showed Niko's
pistol next to Minecraft's sword; one a script hands him too) and comes back in Niko mode; in a
vehicle in Minecraft mode too (GTA picked the pistol for drive-bys there and its HUD showed it
next to the hotbar), and drive-bys are off (`SET_PLAYER_CAN_DO_DRIVE_BY`) until Niko mode. While GTA
drives the player (`HostDriveClient.driving()`) Minecraft draws no first-person hands or held items
into the overlay (`GameRendererHandMixin`), and its HUD keeps only the hotbar (selected item,
offhand), the hearts and the food bar (`HudWhileGtaDrivesMixin`: no crosshair or attack indicator,
armour, experience/locator/jump bar and level, air, mount hearts, action bar such as "Press Left
Shift to dismount", titles, held item name, effects, boss bars or camera overlays; chat, the
scoreboard, subtitles and toasts stay); the host draws no crosshair invert pass then either
(`kFrameCrosshair` needs puppet mode). Focus loss and GTA menus send
`kInReleaseAll`.

GTA's phone (`PhoneKeys=1`, the default) works like in plain GTA IV: Up takes it out (or answers a
call), the arrows navigate, Enter selects, Backspace goes back and finally puts it away. The arrow
keys are always GTA's; while the phone is out Enter, Backspace and the number keys are GTA's too
(Minecraft gets none of them then, only key-ups). In the pad, measured with `DebugPhone`: Up feeds
`PHONE_TAKE_OUT`, `FRONTEND_UP`, `KB_UP`; the other arrows the matching `FRONTEND_`/`KB_` controls;
Enter `FRONTEND_ACCEPT` and `KB_PHONE_ACCEPT`; Backspace `FRONTEND_CANCEL` and `KB_PHONE_CANCEL`.
Those stay unzeroed: the arrows' controls and the phone's own accept/cancel always, the frontend
accept/cancel only while the phone is out. "The phone is out" is the game's own flag that
`CREATE_MOBILE_PHONE` / `DESTROY_MOBILE_PHONE` set (1.0.8.0: `0x1792CB5`, its address read from the
native's code, which is checked first), or `SCRIPT_IS_USING_MOBILE_PHONE`'s (the next byte); the
log says `phone: out` / `phone: away`. While it is out the puppet move keeps Niko's tasks (his
phone task), which every other puppet frame clears.

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

A trip to the blocky city and back:

    [city] blocky city: Minecraft's player went through a portal (kMcBlockyCity): hiding GTA's map geometry (building pool N slots, object pool N)
    [city] blocky city: GTA's map hidden: N buildings and N map objects in the pools, N hidden so far (+N this frame), scan ... ms
    [city] blocky city: GTA's map shown again (N of N hidden entities still there)
    [city] blocky city: Minecraft's player is back in the mirror world (N scans, ... ms average, ... ms worst)

`ERROR` / `WARNING` mark problems; an unsupported exe is a banner of `ERROR` lines.
