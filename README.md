# LibertyCraft — Minecraft inside GTA IV

Play Grand Theft Auto IV as a Minecraft player. Minecraft (the real Java Edition, running hidden
in the background via Fabric) owns your movement, inventory, hotbar and the blocks you place and
break; GTA IV owns the camera and the picture, and draws the blocks inside Liberty City.

This is a port of the idea (and most of the Minecraft-side code) of
[SkyCraft](https://github.com/chasmlol/SkyCraft) by chasm, which does the same for Skyrim.

> **Status: early development.** Nothing is playable yet. See [Milestones](#milestones).

## How it works

```
 ┌──────────────────────────┐   shared-memory file (/dev/shm)   ┌──────────────────────────────┐
 │ Minecraft 26.3 + Fabric  │ ───── block meshes, HUD, state ──▶ │ GTA IV 1.0.8.0 (Proton)      │
 │ hidden window            │ ◀──── input, camera, collision ─── │ LibertyCraft.asi (IV-SDK)     │
 │ owns player physics,     │                                    │ draws blocks with D3D9,      │
 │ world, inventory         │                                    │ puppets Niko, samples terrain │
 └──────────────────────────┘                                    └──────────────────────────────┘
```

* `fabric/` — the Minecraft mod (fork of SkyCraft's, MIT).
* `asi/` — the GTA IV plugin, built on [IV-SDK](https://github.com/Zolika1351/iv-sdk) (GPL-3.0).
* `protocol/` — the shared-memory protocol both sides implement (byte-compatible with SkyCraft v11).
* `tools/` — install/downgrade, build and launch scripts, plus Python stand-ins for either side.

## Requirements

* Linux with Steam + Proton (developed on Arch, Proton 11). Windows should work too but is untested.
* **GTA IV: The Complete Edition** on Steam. The install script downgrades it to **1.0.8.0** (the
  version IV-SDK and ZolikaPatch support) and installs FusionFix + ZolikaPatch.
* **Minecraft: Java Edition** (owned) with [Prism Launcher](https://prismlauncher.org/) and a
  Minecraft account signed in. Java 25 is picked up from Prism's own runtimes.
* For the scripts: `bash`, `curl`, `unzip`, `python3`.
* To build from source: `xwin`, `clang` (clang-cl), `lld`, `llvm` (llvm-rc), `cmake`, `ninja`, a JDK 25
  (Gradle fetches the rest). `tools/setup-toolchain.sh` checks them and prints the install line, e.g.
  `paru -S xwin clang lld cmake ninja jq 7zip unzip curl python jdk-openjdk`.

## Setup

```sh
tools/install.sh        # 1. downgrade GTA IV, install the ASI loader, ZolikaPatch, FusionFix,
                        #    create the "LibertyCraft" Prism instance (Minecraft 26.3 + Fabric)
tools/setup-toolchain.sh && tools/build.sh --install
                        # 2. build LibertyCraft.asi + the Fabric mod and copy them in place
tools/launch.sh         # 3. start Minecraft (hidden) and GTA IV
```

Every script takes `--dry-run` (show what would happen) and `--help`; `install.sh` and `build.sh`
also take `--yes`. Re-running any of them is safe.

What `tools/install.sh` does to the game folder (`…/steamapps/common/Grand Theft Auto IV/GTAIV`):

* Replaces `GTAIV.exe` with 1.0.8.0 plus its scripts/text (`script.img`, `*.gxt`, `PlayGTAIV.exe`,
  `play.dll`), from [Gillian's downgrade assets](https://github.com/gillian-guide/GTAIVFullDowngradeAssets)
  — the same defaults as Gillian's GTA IV Downgrade Utility. `--full` also copies the complete
  1.0.8.0 file set.
* Installs Ultimate ASI Loader **as `xlive.dll`** (FusionFix Legacy Addon). 1.0.8.0 needs Games for
  Windows Live's `xlive.dll`; this one replaces GFWL and loads the `.asi` plugins, so no
  `dinput8.dll` and no `WINEDLLOVERRIDES` are needed.
* Installs ZolikaPatch (with the options FusionFix already covers switched off), FusionFix
  (`plugins/`, `update/`), XLivelessAddon and — once built — `plugins/LibertyCraft.asi`.
* Backs up every file it replaces to `_libertycraft_backup/` with a manifest;
  `tools/uninstall.sh` (= `install.sh --uninstall`) restores them and deletes what was added.
  Steam → GTA IV → Properties → Installed Files → **Verify integrity of game files** also reverts
  the downgrade (and so does a Steam update of the game — just run `tools/install.sh` again).

Recommended Steam launch options (Properties → General): `PROTON_LOG=1 %command%` while developing
(writes `~/steam-12210.log`). Logs: Minecraft `…/PrismLauncher/instances/LibertyCraft/minecraft/logs/latest.log`,
the plugin `<gamedir>/LibertyCraft.log`. `tools/dev/run-gta-with-log.sh` starts the game through Proton
directly with `PROTON_LOG=1`, without touching Steam's launch options.

## Milestones

- [ ] **M0** — repo, scripted downgrade, hello-world plugin loads in-game
- [ ] **M1** — Fabric mod runs on Linux against the Python stand-in host
- [ ] **M2** — Niko moves with Minecraft physics; mouse look; keyboard forwarded
- [ ] **M3** — blocks and the Minecraft HUD rendered inside Liberty City
- [ ] **M4** — real collision with buildings, stairs, water
- [ ] **M5** — explosions, peds as mobs' targets, lights, polish

## Licenses

* Everything outside `asi/` is **MIT** (see `LICENSE`), including the Fabric mod (derived from SkyCraft, MIT).
* `asi/` (the GTA IV plugin) is **GPL-3.0** (see `asi/LICENSE`) because it is built on IV-SDK, which is GPL-3.0.
* Third-party notices: `THIRD-PARTY-NOTICES.md`.

## Credits

* [chasm — SkyCraft](https://github.com/chasmlol/SkyCraft): the idea, the protocol and the Minecraft side.
* [Zolika1351 — IV-SDK](https://github.com/Zolika1351/iv-sdk) and ZolikaPatch.
* [ThirteenAG — FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) and Ultimate ASI Loader.
* [Gillian's guide](https://gillian-guide.github.io/) for the downgrade assets and procedure.
