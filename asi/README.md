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
| `HostDrive.*`, `DriveLogic.h` | vehicles and Niko mode: who drives the player (Minecraft or GTA IV) |
| `Collision.*` | v1 ground heightfield -> kColTris / kColRegion (region scheduler from SkyCraft) |
| `Render.*`, `Overlay.*` | stubs: drain the render ring, acquire overlay frames, count them |
| `Coords.h` | GTA <-> MC coordinates, heading <-> yaw, camera basis |
| `Config.*`, `Log.*`, `CrashLog.*`, `Perf.h` | ini, log file, crash handler, frame-time stats |

Hooks: `processScriptsEvent` -> `Game::Tick` (natives), `processCameraEvent` -> `Game::Camera`
(after the final camera is written), `processPadEvent` -> `Input::Pad`, `drawingEvent` ->
`Render::Draw`, `ingameStartupEvent` -> `Game::OnIngameStartup`. A background thread beats the
host heartbeat every 250 ms (through loading screens, the pause menu and alt-tab) and reopens
the bridge file if Minecraft deleted/replaced it.

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
- Camera matrix: which `CMatrix` row (`right`/`up`/`at`) is the camera's right / forward / up is
  discovered at runtime from the game's own on-foot camera (logged as `camera rows discovered`);
  pin it with `CameraRows=` once known.
- Feet: `GET_CHAR_COORDINATES` is the ped's root (~1 m above the soles). The plugin measures
  root z - ground z while the player stands still (logged as `root->feet measured`) and uses
  `RootToFeet` (1.0) until then; puppeting sets the root to MC feet + that offset with
  `SET_CHAR_COORDINATES_NO_OFFSET`.

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
| `ProbeFrom` | `top` | collision heightfield probe: `top` (1000 m: buildings are solid to the roof) or `feet` |
| `ProbeHeight` | `3.0` | with `ProbeFrom=feet`: start the probe this far above the feet |
| `CameraRows` | `auto` | `auto` or e.g. `0,1,2` / `-0,1,2` (right, forward, up as CMatrix rows; `-` flips) |
| `VehicleKey` | `F` | while Minecraft drives: GTA enters/steals the nearest vehicle (see Vehicles and Niko mode) |
| `ToggleKey` | `Backslash` | Minecraft mode <-> Niko mode. Key names: a letter, digit, F1-F12, `Backslash`, `Grave`, `Tab`, `Minus`, `Equals`, `LBracket`, `RBracket`, `Semicolon`, `Apostrophe`, `Comma`, `Period`, `Slash`, `Space`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Numpad0`-`9`, ... or a DIK code like `0x2B` |
| `HideNikoInVehicle` | `1` | hide Niko in vehicles in Minecraft mode (Minecraft's player rides its mount there) |
| `ToggleStartsInMinecraft` | `1` | start in Minecraft mode (0: Niko mode) |
| `VehicleSeatDrop` | `0.75` | metres from the seated ped's position down to the riding Minecraft player's feet |
| `VehicleEnterFallback` | `warp` | GTA's enter press didn't take within 2 s: `warp` (`WARP_CHAR_INTO_CAR`, closest car within 10 m), `task` (`TASK_ENTER_CAR_AS_DRIVER`), `none` |
| `DebugAutoToggle` | `0` | test hook (not in the default ini): toggle the mode every 10 s |
| `DebugAutoVehicle` | `0` | test hook: press the vehicle key when a car is within 12 m (else park an empty test car next to Niko first; indoors, move him to the nearest road), and GTA's exit control after 12 s in a car |

## Vehicles and Niko mode

GTA IV takes the player back from Minecraft (`kSkyHostDrives`: Minecraft follows `SkyState`
pos/yaw with no physics, input or damage) when:

- **Niko mode** (`ToggleKey`, Backslash): plain GTA IV. No puppet, no input for Minecraft, GTA's
  HUD and mouse; Minecraft's player follows Niko. The toggle key works in both modes (it is
  never forwarded) and shows a short on-screen note. Back in Minecraft mode the teleport
  handshake runs at Niko's spot, then puppet mode.
- **`VehicleKey`** (F) while puppeting (never forwarded to Minecraft): puppet mode lets go and
  `processPadEvent` holds GTA's own `INPUT_ENTER` for 0.3 s, so the game picks the door or
  carjacks as usual. No "getting in" after 2 s: `VehicleEnterFallback`. No vehicle after 4 s:
  back to Minecraft.
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
from window messages. GTA reads DirectInput itself, so `processPadEvent` zeroes every CPad
control except `INPUT_FRONTEND_PAUSE`. Focus loss and GTA menus send `kInReleaseAll`.

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
    [game] camera rows discovered: camera right = right, forward = up, up = at ...
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
    [render] render ring drained ... KiB in 1.0s (...): section ... ; overlay frames acquired ...

`ERROR` / `WARNING` mark problems; an unsupported exe is a banner of `ERROR` lines.
