# CLAUDE.md

Guidance for Claude Code (and its subagents) working in this repository.

## Writing rules

- Never use em dashes (U+2014) or en dashes (U+2013), anywhere: code comments, commit messages,
  docs, READMEs, logs, reports and chat.
- Do not use a hyphen surrounded by spaces as a dash either. Split the thought into two sentences,
  or use a comma, a colon or parentheses. Ranges are written with "to" (F1 to F12, 1.0 to 2.5 m).
- Hyphens inside words and identifiers are fine (x86-64, clang-cl, read-only, `--dry-run`).
- Before committing, check: `git diff --cached | grep -nP '[\x{2013}\x{2014}]'` must print nothing.
- Commit messages: conventional commits (`feat(asi): ...`, `fix(tools): ...`). Never add
  Co-Authored-By, "Generated with" or any other AI attribution.

## What this project is

LibertyCraft runs Minecraft inside GTA IV, a port of [SkyCraft](https://github.com/chasmlol/SkyCraft)
(Minecraft in Skyrim). Real Minecraft (Java 26.3, Fabric) runs hidden and owns the player's physics,
world, inventory and combat. GTA IV 1.0.8.0 (Steam, under Proton) shows the picture: its plugin draws
Minecraft's blocks and HUD, puppets Niko, and streams GTA's collision back to Minecraft.

| Path | What | License |
|---|---|---|
| `fabric/` | Minecraft mod (fork of SkyCraft's), Java 25, Gradle | MIT |
| `asi/` | GTA IV plugin `LibertyCraft.asi`, C++17 on IV-SDK (submodule `asi/extern/iv-sdk`) | GPL-3.0 |
| `protocol/libertycraft_protocol.h` | shared-memory protocol, mirrored by hand in `fabric/.../link/Proto.java` | MIT |
| `tools/` | install/downgrade, build, launch scripts; Python stand-ins for either side | MIT |

`asi/README.md` has the plugin's file table, ini keys and expected log lines. Read it before
changing the plugin.

## Hard constraints

- **Protocol layout is frozen** (byte-identical to SkyCraft v11, `kVersion` 11). Only add flag bits
  or documented conventions. If a field must change, change the header, `Proto.java` and the Python
  stand-ins together and bump the version.
- **Coordinates:** GTA (x east, y north, z up, metres) to Minecraft (x east, y up, z south):
  `mc = (x, z, -y)`, `gta = (x, -z, y)`, 1 block = 1 m. Helpers live in `asi/src/Coords.h`.
- **Transport:** the file `/dev/shm/libertycraft-bridge` (191 MiB). Java maps it natively; the Wine
  DLL opens `Z:\dev\shm\libertycraft-bridge`. Only ever grow it, never truncate it.
- **Clocks:** heartbeats are `GetTickCount64` ms, tick stamps are QPC (100 ns). Under Wine both are
  `CLOCK_MONOTONIC_RAW`, which is what the Java side reads.
- **32-bit plugin:** keep atomically shared fields 32-bit or protected by a seqlock; the address space
  is small, so avoid large contiguous allocations.

## Toolchain and commands

Arch Linux, no Windows needed. Never install system packages (no paru/pacman/sudo); if a tool is
missing, tell the user the `paru -S ...` line and stop.

```sh
tools/setup-toolchain.sh                 # checks clang-cl, lld-link, cmake, ninja, xwin; builds .tools/xwin
cd asi && cmake --preset release && cmake --build --preset release   # -> asi/build/release/LibertyCraft.asi
cmake -S asi/tests -B asi/build/tests && cmake --build asi/build/tests && ctest --test-dir asi/build/tests
cd fabric && JAVA_HOME=~/.local/share/PrismLauncher/java/java-runtime-epsilon ./gradlew build test --no-daemon
tools/build.sh --install --yes           # build both and install into the game + Prism (game must be closed)
tools/install.sh                         # downgrade GTA IV, FusionFix, ZolikaPatch, Prism instance (idempotent)
tools/launch.sh                          # Minecraft first, GTA IV once Minecraft's window is hidden
```

## Game and environment facts

- Game dir: `~/.local/share/Steam/steamapps/common/Grand Theft Auto IV/GTAIV/`. Plugin log
  `LibertyCraft.log` there; config `plugins/LibertyCraft.ini`.
- Minecraft log: `~/.local/share/PrismLauncher/instances/LibertyCraft/minecraft/logs/latest.log`.
- Loader is Ultimate ASI Loader as `xlive.dll`. Do not install XLivelessAddon (it makes the game loop
  on "connection to Games for Windows LIVE has been lost"). Keep ZolikaPatch on with `MiscFixes=0` and
  `BikeFeetFix=0` (both crash next to FusionFix 5.x).
- Launched through Steam the game process is `PlayGTAIV.exe`; through `tools/dev/run-gta-with-log.sh`
  it is `GTAIV.exe`. Check with `ps -eo args | grep -qE '^[SZ]:.*\\(Play)?GTAIV\.exe'`.
- `pgrep -f`/`pkill -f` also match your own shell when the pattern text is in your command line. Put
  kill logic in a script file; never inline the pattern in the command that writes it.
- Start Minecraft first and GTA IV only after Minecraft logs `game window hidden`: GTA IV stops
  loading while another window has the focus.
- Screenshots (KDE Wayland): launch the game through Steam (the overlay makes it composited), then
  `spectacle --background --nonotify --fullscreen --output FILE.png`. The game monitor is DP-3 at
  desktop offset (0,360): `magick FILE.png -crop 1920x1080+0+360 out.png`. Launched outside Steam,
  captures are black. There is no input injection, so tests must not need key presses.
- New games start in Roman's apartment in Broker; puppet mode only engages on foot.

## Working with agents and in-game tests

- Several agents may work at once. Edit only your own files; keep edits to shared files (Game.cpp,
  Config.*, CMakeLists.txt, dllmain.cpp, README) small and additive, and re-read them before editing.
- One game at a time: wrap an in-game session in `flock /tmp/claude-1000/libertycraft-gta.lock`, wait
  while `/tmp/claude-1000/libertycraft-user-testing` exists (the user is playing), close the game and
  any Minecraft you started when done, and restore any ini you changed.
- Run subagents on Opus (`model: "opus"`); the user watches credit usage.
- Commit and push to `origin main` (github.com/mrborghini/libertycraft) at each milestone, after the
  plugin and the mod build and all tests pass.
