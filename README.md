# LibertyCraft: Minecraft inside GTA IV

Play Grand Theft Auto IV as a Minecraft player. Minecraft (the real Java Edition, running hidden
in the background via Fabric) owns your movement, inventory, hotbar and the blocks you place and
break; GTA IV owns the camera and the picture, and draws the blocks inside Liberty City.

This is a port of the idea (and most of the Minecraft-side code) of
[SkyCraft](https://github.com/chasmlol/SkyCraft) by chasm, which does the same for Skyrim.

> **Status: early development, but playable.** Minecraft drives Niko, its blocks and HUD render in
> Liberty City with GTA's lighting, collision follows GTA IV's map, combat and crimes work both ways, and
> your Minecraft character plays Niko in cutscenes and cars. See [Milestones](#milestones).

## Screenshots

![A Minecraft cabin, TNT and a pig on a Broker street under the El train](docs/screenshots/broker-build.jpg)
*Real Minecraft blocks and mobs on a Broker street, lit by GTA IV's sun and hidden behind its world.*

| | |
|---|---|
| ![TNT in Roman's apartment](docs/screenshots/apartment-tnt.jpg) | ![Blocky Liberty City, a block copy of Broker drawn by GTA IV](docs/screenshots/blocky-city.jpg) |
| Blocks indoors, with the Minecraft HUD | Through a nether portal: Blocky Liberty City, with GTA's sky and traffic |
| ![Driving a convertible as the Minecraft player](docs/screenshots/driving.jpg) | ![Bailing out of a moving car](docs/screenshots/bailout.jpg) |
| Driving (F steals a car the GTA way) | Bailing out of a moving car: GTA's ragdoll, Minecraft's body |
| ![Minecraft water flooding a Broker street](docs/screenshots/flood.jpg) | ![A car wrecked by crossbow fire](docs/screenshots/car-wreck.jpg) |
| Minecraft water floods the street and stalls the cars | Crossbow fire wrecks a car (fireworks bring down helicopters) |

## How it works

```
 ┌──────────────────────────┐   shared-memory file (/dev/shm)   ┌──────────────────────────────┐
 │ Minecraft 26.3 + Fabric  │ ───── block meshes, HUD, state ──▶ │ GTA IV 1.0.8.0 (Proton)      │
 │ hidden window            │ ◀──── input, camera, collision ─── │ LibertyCraft.asi (IV-SDK)     │
 │ owns player physics,     │                                    │ draws blocks with D3D9,      │
 │ world, inventory         │                                    │ puppets Niko, samples terrain │
 └──────────────────────────┘                                    └──────────────────────────────┘
```

* `fabric/`: the Minecraft mod (fork of SkyCraft's, MIT).
* `asi/`: the GTA IV plugin, built on [IV-SDK](https://github.com/Zolika1351/iv-sdk) (GPL-3.0).
* `protocol/`: the shared-memory protocol both sides implement (byte-compatible with SkyCraft v11).
* `tools/`: install/downgrade, build and launch scripts, plus Python stand-ins for either side.

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
  (the same defaults as Gillian's GTA IV Downgrade Utility). `--full` also copies the complete
  1.0.8.0 file set.
* Installs Ultimate ASI Loader **as `xlive.dll`** (FusionFix Legacy Addon). 1.0.8.0 needs Games for
  Windows Live's `xlive.dll`; this one replaces GFWL and loads the `.asi` plugins, so no
  `dinput8.dll` and no `WINEDLLOVERRIDES` are needed.
* Installs ZolikaPatch (with the options FusionFix already covers switched off), FusionFix
  (`plugins/`, `update/`) and, once built, `plugins/LibertyCraft.asi`.
* With `--radio` (optional, ~1 GB download) it also installs Tomasak's
  [Radio Restoration Mod](https://github.com/Tomasak/GTA-Downgraders/releases/tag/iv-latest), which brings back the
  songs removed from the Steam release, into `update/`. `--radio=vanilla` keeps only the original tracklist; see
  `tools/install.sh --help` for the other variants.
* Backs up every file it replaces to `_libertycraft_backup/` with a manifest;
  `tools/uninstall.sh` (= `install.sh --uninstall`) restores them and deletes what was added.
  Steam → GTA IV → Properties → Installed Files → **Verify integrity of game files** also reverts
  the downgrade (and so does a Steam update of the game; just run `tools/install.sh` again).

Recommended Steam launch options (Properties → General): `PROTON_LOG=1 %command%` while developing
(writes `~/steam-12210.log`). Logs: Minecraft `…/PrismLauncher/instances/LibertyCraft/minecraft/logs/latest.log`,
the plugin `<gamedir>/LibertyCraft.log`. `tools/dev/run-gta-with-log.sh` starts the game through Proton
directly with `PROTON_LOG=1`, without touching Steam's launch options.

## Controls

In **Minecraft mode** (the default) you play Minecraft: its keys, mouse, hotbar and inventory work
as usual (`E` inventory, `T` chat, `F5` third person). GTA IV only keeps a few keys:

| Key | What it does |
|---|---|
| `F` | enter or steal the nearest car the GTA way. You drive with GTA's controls and the Minecraft player rides a boat in the seat; `F` again gets out and Minecraft takes over |
| `\` (Backslash) | switch between Minecraft mode and **Niko mode** (plain GTA IV, e.g. if something misbehaves); the Minecraft player follows Niko and takes over where he stands |
| `O` | Minecraft's pause / options menu |
| `Esc`, `F1` to `F12`, `` ` `` | GTA IV's own (pause menu, ...) |

Keys and the boat/horse/minecart mount can be changed in `GTAIV/plugins/LibertyCraft.ini` and
Minecraft's `config/libertycraft.properties`.

## Blocky Liberty City

Build a nether portal frame of obsidian anywhere in Liberty City and light it: it leads to **Blocky
Liberty City**, a Minecraft dimension (`libertycraft:blocky_city`) that copies the city block for block,
at the same coordinates (1:1), built from the collision GTA IV has streamed to Minecraft. Every street
you walk down in GTA IV is remembered in the world save (`saves/LibertyCraft/libertycraft_city/`, about
150 KiB per street block, written in the background), so the copy grows as you explore, also while you walk
around in it; where nobody has been yet there is nothing but air behind an invisible wall. GTA IV's own
surface materials pick the blocks: tarmac becomes gray concrete, pavements smooth stone, brick walls
bricks, glass glass, grass grass, roofs gray concrete, metal iron, wood planks; GTA IV's water becomes
Minecraft water. Parts built before newer data arrived fill in when you are there (what is already
built stays, so your own builds are safe).

You stay in GTA IV: it hides its own buildings, terrain and props while you are in the blocky city and
draws the city's blocks in their place, around the same streets, with its sky, weather, water, people,
traffic and HUD. Peds and cars still walk and drive on GTA IV's own (invisible) city, which the blocks
copy. A nether portal there (the one you arrived through, or one you build) leads back to the same spot
in Liberty City, and GTA IV's map comes back.

`blockyCity=false` in Minecraft's `config/libertycraft.properties` makes nether portals do nothing.

## Milestones

- [x] **M0**: repo, scripted downgrade (`tools/install.sh`), plugin loads in-game under Proton
- [x] **M1**: Fabric mod runs on Linux; Minecraft links to GTA IV through `/dev/shm`
- [x] **M2**: Niko moves with Minecraft physics; mouse look; keyboard forwarded
- [x] **M3**: Minecraft blocks rendered in Liberty City (lit like Minecraft, hidden behind GTA's buildings) and the Minecraft HUD composited over the game
- [x] **M4**: collision from GTA IV's real map geometry: streets, stairs, interiors, overpasses, walls; water for swimming
- [x] **M5**: combat: Minecraft hits damage/ragdoll/kill peds, TNT and creepers make GTA explosions, GTA damage hurts the Minecraft player, death runs GTA's "wasted" flow
- [x] **M6**: cars (F steals one the GTA way, the Minecraft player rides a boat in it) and Backslash to switch between Niko and Minecraft
- [x] **M7**: your Minecraft character plays Niko when GTA animates him (cutscenes, cars, bail-outs, knockdowns);
  doors open, street furniture is solid, NPCs and cops fight back, Minecraft attacks count as crimes, cars and
  their occupants can be hit, optional radio restoration
- [x] **M8**: blocks and your Minecraft body take and cast GTA's sun shadows (same cascades and filtering as GTA),
  both HUDs at once with GTA's health and armour mirroring Minecraft, shields block GTA hits, GTA's phone works in
  Minecraft mode
- [x] **M9**: Minecraft blocks stop GTA's bullets, Minecraft attacks feed GTA's own crime system (wanted levels
  escalate), killing blows ragdoll, Minecraft fire and lava burn peds and cars, cars slow and stall in Minecraft
  water, a trimmed Minecraft HUD in vehicles
- [x] **M10**: walking into peds pushes them (sprinting makes them stumble), elytra, falls and sprint-jumps knock
  them down by speed, corpses can be hit and dragged, doors swing open and shut on GTA's own hinges, every vehicle
  (helicopters and their rotors too) knocks you over, and entering interiors no longer drops Minecraft mode
- [x] **M11**: fireworks fired from a crossbow explode like GTA rockets (they bring down helicopters), nether
  portals lead to Blocky Liberty City, softer knockdowns and corpse pushes, cops shoot back at the Minecraft player,
  held items hidden in vehicles, no void when the camera is outside an interior's door

Known gaps: no shadows on blocks indoors; peds wade instead of swim in Minecraft water; cutscene support needs
1.0.8.0; peds take double damage from Minecraft's TNT and creepers (not from fireworks). Blocky Liberty City is drawn
128 m out, loses lamp posts and other thin street furniture, and can sit up to half a block off GTA's ground.
GTA's own ladders can't be climbed in Minecraft mode (Minecraft ladders can). (Windscreens can't be broken in vanilla GTA IV either, so only side windows shatter.)

## Licenses

* Everything outside `asi/` is **MIT** (see `LICENSE`), including the Fabric mod (derived from SkyCraft, MIT).
* `asi/` (the GTA IV plugin) is **GPL-3.0** (see `asi/LICENSE`) because it is built on IV-SDK, which is GPL-3.0.
* Third-party notices: `THIRD-PARTY-NOTICES.md`.

## Credits

* [SkyCraft by chasm](https://github.com/chasmlol/SkyCraft): the idea, the protocol and the Minecraft side.
* [IV-SDK by Zolika1351](https://github.com/Zolika1351/iv-sdk) and ZolikaPatch.
* [FusionFix by ThirteenAG](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) and Ultimate ASI Loader.
* [Gillian's guide](https://gillian-guide.github.io/) for the downgrade assets and procedure.
