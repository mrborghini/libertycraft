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
  version the modding SDKs support) and installs FusionFix + ZolikaPatch.
* **Minecraft: Java Edition** (owned) with [Prism Launcher](https://prismlauncher.org/).
* To build from source: `clang`/`lld`, `cmake`, `ninja`, `xwin`, a JDK 21+ (Gradle downloads the rest).

## Setup

_Coming with the first milestone._ The intended flow is three steps:

1. `tools/install.sh` — downgrades GTA IV, installs FusionFix/ZolikaPatch, creates the Prism instance.
2. `tools/build.sh` — builds the plugin and the mod (or download a release).
3. `tools/launch.sh` — starts Minecraft and GTA IV together.

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
