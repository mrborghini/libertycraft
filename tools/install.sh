#!/usr/bin/env bash
# tools/install.sh - get GTA IV and Minecraft ready for LibertyCraft.
#
#  1. Downgrades the Steam Complete Edition (1.2.0.x) to 1.0.8.0, the version IV-SDK,
#     ZolikaPatch and LibertyCraft.asi are written against. Same files and defaults as
#     Gillian's GTA IV Downgrade Utility.
#  2. Installs the ASI loader (Ultimate ASI Loader as xlive.dll), ZolikaPatch, FusionFix
#     and - if built - LibertyCraft.asi. (Not XLivelessAddon: see step 4.)
#  3. Creates/updates the "LibertyCraft" Prism Launcher instance (Minecraft 26.3 + Fabric).
#
# Everything it changes in the game folder is backed up and listed in
# <gamedir>/_libertycraft_backup/manifest.txt, so `--uninstall` can put it all back.
# Safe to run again: unchanged files are skipped, backups are never overwritten.
set -euo pipefail
# shellcheck source=lib/gamefiles.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/gamefiles.sh"

usage() {
  cat <<EOF
Usage: tools/install.sh [options]

  -n, --dry-run        show what would happen; change nothing
  -y, --yes            do not ask for confirmation
      --uninstall      undo everything recorded in the backup manifest (see below)
      --full           also copy the full 1.0.8.0 data files (1080FullFiles.zip, ~100 MB);
                       only needed if something misbehaves with the minimal downgrade
      --no-fusionfix   skip FusionFix (the ASI loader is still installed)
      --no-zolikapatch skip ZolikaPatch
      --no-minecraft   skip the Prism Launcher instance
      --dxvk           also install FusionFix's d3d9.dll/vulkan.dll (its own DXVK build).
                       Not needed under Proton, which already runs D3D9 through DXVK.
      --force-clean    move *other* existing .asi plugins, plugins/ files and known
                       incompatible DLLs out of the game folder (into the backup)
      --refresh-assets re-download the unpinned FusionFix zips (they track "latest")
  -v, --verbose        list every file
  -h, --help           this help

Environment: GTAIV_DIR (folder with GTAIV.exe), PRISM_DIR, JAVA25_HOME, LIBERTYCRAFT_CACHE.
EOF
}

FULL=0 FUSIONFIX=1 ZOLIKA=1 MINECRAFT=1 DXVK=0 FORCE_CLEAN=0 UNINSTALL=0 REFRESH=0 VERBOSE=0
while (( $# )); do
  case $1 in
    -n|--dry-run)      DRY_RUN=1 ;;
    -y|--yes)          ASSUME_YES=1 ;;
    --uninstall)       UNINSTALL=1 ;;
    --full)            FULL=1 ;;
    --no-fusionfix)    FUSIONFIX=0 ;;
    --no-zolikapatch)  ZOLIKA=0 ;;
    --no-minecraft)    MINECRAFT=0 ;;
    --dxvk)            DXVK=1 ;;
    --force-clean)     FORCE_CLEAN=1 ;;
    --refresh-assets)  REFRESH=1 ;;
    -v|--verbose)      VERBOSE=1 ;;
    -h|--help)         usage; exit 0 ;;
    *) usage >&2; die "Unknown option: $1" ;;
  esac
  shift
done
export VERBOSE

# --- pinned facts -----------------------------------------------------------------------
TARGET_VERSION=1.0.8.0
TARGET_EXE_SIZE=15628696
# Gillian's downgrade assets: a fixed release (2023-12-31), so the checksums are pinned.
BASE_URL=https://github.com/gillian-guide/GTAIVFullDowngradeAssets/releases/download/Base
BASE_SUM=sha256:dff2ad5da752157c466f7d5721a19132ac42a41298f959b4f89243e31c95150b
FULL_SUM=sha256:0dbb0f5f7895bc3dff299d4cbdfa74181fd525cb48323393f0a1e55ac57dd956
# FusionFix is a rolling "latest" release: verified with `unzip -t` + expected entries instead.
FF_URL=https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix/releases/latest/download
FABRIC_API_JAR=fabric-api-0.161.0+26.3.jar
FABRIC_API_URL=https://cdn.modrinth.com/data/P7dR8mSH/versions/bNnaTiuM/fabric-api-0.161.0%2B26.3.jar
FABRIC_API_SUM=sha512:ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d
MC_VERSION=26.3
LOADER_VERSION=0.19.5
LWJGL_VERSION=3.4.3
JVM_ARGS="--enable-native-access=ALL-UNNAMED -Dlibertycraft.startHidden=true"

# ZolikaPatch options that FusionFix already provides (or that fight with it): the list the
# downgrade utility switches off when FusionFix is selected, plus BikeFeetFix - FusionFix 5.x
# ships the same patch (fixes.ixx) and patching it twice crashes 1.0.8.0 ~4 s after start
# (verified by bisecting all 55 enabled options under Proton 11).
ZOLIKA_OFF_FF=(BikeFeetFix BikePhoneAnimsFix BorderlessWindowed BuildingAlphaFix BuildingDynamicShadows
  CarDynamicShadowFix CarPartsShadowFix CutsceneFixes DoNotPauseOnMinimize DualVehicleHeadlights
  EmissiveLerpFix EpisodicVehicleSupport EpisodicWeaponSupport ForceCarHeadlightShadows
  ForceDynamicShadowsEverywhere ForceShadowsOnObjects HighFPSBikePhysicsFix HighFPSSpeedupFix
  HighQualityReflections ImprovedShaderStreaming MouseFix NewMemorySystem NoLiveryLimit
  OutOfCommissionFix PoliceEpisodicWeaponSupport RemoveBoundingBoxCulling ReversingLightFix
  SkipIntro SkipMenu)
# Always off: with FusionFix 5.x it crashes the game ~2 s after FusionFix loads (access violation
# inside GTAIV.EFLC.FusionFix.asi; reproduced under Proton 11, 2026-10). ZolikaPatch.ini describes
# it as "some features from XLivelessAddon"; we need neither (see step 4).
ZOLIKA_OFF_ALWAYS=(MiscFixes)

# --- helpers ----------------------------------------------------------------------------
STAGE=""
cleanup() { [[ -n $STAGE && -d $STAGE ]] && rm -rf -- "$STAGE"; return 0; }
trap cleanup EXIT

# unpack ZIP DEST PATTERN... : extract selected entries into the staging dir.
unpack() {
  local zip=$1 dest=$2; shift 2
  mkdir -p -- "$dest"
  unzip -qo "$zip" "$@" -d "$dest"
}

need_asset() { [[ ${HAVE[$1]:-0} == 1 ]]; }

# --- 0. preflight -----------------------------------------------------------------------
(( DRY_RUN )) && printf '%sDry run: nothing will be changed.%s\n' "$C_BOLD$C_CYAN" "$C_RESET"
step "Preflight"
require_tools unzip:unzip curl:curl python3:python sha256sum:coreutils cmp:diffutils
GAME=$(find_gta_dir) || die "GTA IV (Steam app $LC_STEAM_APPID) not found. Set GTAIV_DIR to the folder containing GTAIV.exe."
ok "game folder: $GAME"
GAME_VER=$(pe_version "$GAME/GTAIV.exe")
case $GAME_VER in
  1.2.*)   ok "GTAIV.exe $GAME_VER (Complete Edition)" ;;
  "$TARGET_VERSION") ok "GTAIV.exe $GAME_VER (already downgraded)" ;;
  *)       warn "GTAIV.exe reports version $GAME_VER - unexpected, continuing anyway" ;;
esac
[[ -w $GAME ]] || die "No write permission for $GAME"
if gta_running; then
  if (( DRY_RUN )); then warn "GTA IV is running (a real run would stop here)"
  else die "GTA IV is running. Quit the game first."; fi
fi
manifest_load "$GAME"
if (( $(manifest_count) )); then info "previous LibertyCraft install found ($(manifest_count) tracked changes)"; fi

# --- uninstall ----------------------------------------------------------------------------
if (( UNINSTALL )); then
  step "Uninstall: restoring $GAME"
  if (( $(manifest_count) )); then
    confirm "Restore the original game files and delete what LibertyCraft added?" || die "Aborted."
    uninstall_game
    (( DRY_RUN )) || ok "GTAIV.exe is now version $(pe_version "$GAME/GTAIV.exe")"
  else
    info "nothing to undo"
  fi
  if INST=$(lc_instance_dir) && [[ -d $INST ]]; then
    info "The Prism instance is left alone (it holds your worlds): $INST"
    info "Delete it from Prism Launcher if you no longer want it."
  fi
  info "Steam -> GTA IV -> Properties -> Installed Files -> \"Verify integrity of game files\""
  info "also brings back the vanilla Complete Edition (it leaves extra files such as plugins/ alone)."
  exit 0
fi

if [[ $GAME_VER == "$TARGET_VERSION" && $(manifest_state GTAIV.exe) == "" ]]; then
  warn "GTAIV.exe was already 1.0.8.0 before LibertyCraft touched it, so --uninstall cannot"
  warn "bring back the Complete Edition exe. Steam's \"Verify integrity\" can."
fi

# --- 1. assets ------------------------------------------------------------------------------
step "Downloading / verifying assets in ${LC_CACHE/#$HOME/\~}"
declare -A HAVE=()
# asset NAME URL CHECKSUM REQUIRED_ENTRY... : fetch into the cache and sanity-check contents
asset() {
  local name=$1 url=$2 sum=$3 e rc=0; shift 3
  if (( REFRESH )) && [[ -z $sum && -f "$LC_CACHE/$name" ]]; then run rm -f -- "$LC_CACHE/$name"; fi
  fetch "$url" "$LC_CACHE/$name" "$sum" || rc=$?
  if (( rc == 2 )); then HAVE[$name]=0; return 0; fi     # dry run, would download
  (( rc == 0 )) || die "Could not get $name"
  for e in "$@"; do
    zip_has "$LC_CACHE/$name" "$e" || die "$name does not contain $e - unexpected archive layout, refusing to continue"
  done
  HAVE[$name]=1
}
asset BaseAssets.zip "$BASE_URL/BaseAssets.zip" "$BASE_SUM" 1080/GTAIV.exe ZolikaPatch/ZolikaPatch.asi Shared/PlayGTAIV.exe
(( FULL )) && asset 1080FullFiles.zip "$BASE_URL/1080FullFiles.zip" "$FULL_SUM" GTAIV.exe
asset GTAIV.EFLC.FusionFixLegacyAddon.zip "$FF_URL/GTAIV.EFLC.FusionFixLegacyAddon.zip" "" xlive.dll plugins/XLivelessAddon.asi
(( FUSIONFIX )) && asset GTAIV.EFLC.FusionFix.zip "$FF_URL/GTAIV.EFLC.FusionFix.zip" "" plugins/GTAIV.EFLC.FusionFix.asi update/update.txt

confirm "Modify $GAME now?" || die "Aborted."
mkdir -p -- "$LC_CACHE"
STAGE=$(mktemp -d "$LC_CACHE/.stage.XXXXXX")

# --- 2. remove what conflicts --------------------------------------------------------------
step "Removing conflicting files"
removed=0
try_remove() { if remove_file "$1"; then info "moved to backup: $1  ($2)"; removed=$((removed + 1)); fi; }
# The Complete Edition's ASI loaders are named dinput8.dll. On 1.0.8.0 the loader is xlive.dll
# (see step 4); a second loader would load every plugin twice.
try_remove dinput8.dll "second ASI loader"
(( ZOLIKA || FUSIONFIX )) && try_remove dsound.dll "ZolikaPatch and FusionFix both refuse to work with it"
# d3d9.dll/vulkan.dll from an earlier --dxvk run: take them out again if --dxvk was dropped.
if (( ! DXVK )); then
  for f in d3d9.dll vulkan.dll; do
    [[ $(manifest_state "$f") == added ]] && try_remove "$f" "installed by an earlier --dxvk run"
  done
fi
# Plugins we did not install (and DLLs the downgrade utility deletes as incompatible).
others=()
shopt -s nullglob
for f in "$GAME"/*.asi "$GAME"/plugins/*.asi "$GAME"/{launc,orig_socialclub,socialclub,1911}.dll; do
  rel=${f#"$GAME"/}
  [[ -f $f && -z $(manifest_state "$rel") ]] && others+=("$rel")
done
shopt -u nullglob
if (( ${#others[@]} )); then
  if (( FORCE_CLEAN )); then
    for rel in "${others[@]}"; do
      try_remove "$rel" "--force-clean"
      [[ $rel == *.asi && -f "$GAME/${rel%.asi}.ini" ]] && try_remove "${rel%.asi}.ini" "--force-clean"
    done
  else
    warn "Other plugins/DLLs found (not installed by LibertyCraft): ${others[*]}"
    warn "They may conflict with this setup. Re-run with --force-clean to move them into the backup."
  fi
fi
(( removed )) || ok "nothing to remove"

# --- 3. downgrade ----------------------------------------------------------------------------
step "Downgrading GTAIV.exe to $TARGET_VERSION"
if need_asset BaseAssets.zip; then
  unpack "$LC_CACHE/BaseAssets.zip" "$STAGE/base" '1080/GTAIV.exe' 'Shared/*' 'ZolikaPatch/ZolikaPatch.*'
  exe="$STAGE/base/1080/GTAIV.exe"
  [[ $(stat -c %s "$exe") == "$TARGET_EXE_SIZE" && $(pe_version "$exe") == "$TARGET_VERSION" ]] \
    || die "BaseAssets.zip's 1080/GTAIV.exe is not the expected $TARGET_VERSION exe"
  install_file "$exe" GTAIV.exe
  ok "GTAIV.exe ($LC_LAST)"
  # Shared/: PlayGTAIV.exe + play.dll (the 1.0.x launcher), script.img and the .gxt text
  # files - 1.0.8.0's scripts and strings, which the CE versions are not compatible with.
  info "1.0.8.0 launcher, scripts and text:"
  install_tree "$STAGE/base/Shared"
else
  plan "copy 1080/GTAIV.exe and Shared/* from BaseAssets.zip (after downloading it)"
fi

if (( FULL )); then
  step "Full 1.0.8.0 data files (--full)"
  if need_asset 1080FullFiles.zip; then
    unpack "$LC_CACHE/1080FullFiles.zip" "$STAGE/full"
    install_tree "$STAGE/full"
  else
    plan "extract all of 1080FullFiles.zip into the game folder (after downloading it)"
  fi
fi

# --- 4. ASI loader ---------------------------------------------------------------------------
step "ASI loader: Ultimate ASI Loader as xlive.dll"
# Why xlive.dll and not dinput8.dll: 1.0.8.0 imports xlive.dll (Games for Windows Live) and
# would not start without GFWL. The Legacy Addon's xlive.dll is Ultimate ASI Loader built to
# stand in for it, so it removes the GFWL dependency *and* loads *.asi from the game root and
# plugins/. As a bonus Wine has no builtin xlive.dll, so no WINEDLLOVERRIDES is needed.
#
# Why not the Legacy Addon's XLivelessAddon.asi: with it the game loops on "The connection to
# Games for Windows - LIVE has been lost. Returning to single player." right after startup
# (with or without its SkipWebConnect option). Without it the game goes straight into the
# story. ZolikaPatch already covers what we would need it for (SavegameFix removes the GFWL
# savegame CRC check, NewSavesCompatibility loads newer saves). Earlier installs added it, so
# take it out again.
if need_asset GTAIV.EFLC.FusionFixLegacyAddon.zip; then
  unpack "$LC_CACHE/GTAIV.EFLC.FusionFixLegacyAddon.zip" "$STAGE/legacy" 'xlive.dll'
  install_tree "$STAGE/legacy"
else
  plan "extract xlive.dll from the Legacy Addon (after downloading it)"
fi
for f in plugins/XLivelessAddon.asi plugins/XLivelessAddon.ini; do
  try_remove "$f" "XLivelessAddon makes the game loop on the GFWL 'connection lost' message"
done

# --- 5. ZolikaPatch -----------------------------------------------------------------------
if (( ZOLIKA )); then
  step "ZolikaPatch"
  if need_asset BaseAssets.zip; then
    install_file "$STAGE/base/ZolikaPatch/ZolikaPatch.asi" ZolikaPatch.asi; ok "ZolikaPatch.asi ($LC_LAST)"
    install_file "$STAGE/base/ZolikaPatch/ZolikaPatch.ini" ZolikaPatch.ini keep; ok "ZolikaPatch.ini ($LC_LAST)"
    off=("${ZOLIKA_OFF_ALWAYS[@]}")
    (( FUSIONFIX )) && off+=("${ZOLIKA_OFF_FF[@]}")
    pairs=(); for k in "${off[@]}"; do pairs+=("$k=0"); done
    ini="$GAME/ZolikaPatch.ini"
    if (( DRY_RUN )); then          # edit a scratch copy to show what would change
      ini="$STAGE/ZolikaPatch.ini"
      if [[ -f "$GAME/ZolikaPatch.ini" ]]; then cp "$GAME/ZolikaPatch.ini" "$ini"; else cp "$STAGE/base/ZolikaPatch/ZolikaPatch.ini" "$ini"; fi
    fi
    res=$(ini_set "$ini" Options "${pairs[@]}")
    ok "ZolikaPatch.ini [Options]: ${#off[@]} options set to 0 ($res)"
    [[ $res == *"missing: -" ]] || warn "some options were not in ZolikaPatch.ini (newer/older ZolikaPatch?) - left alone"
  else
    plan "copy ZolikaPatch.asi/.ini and switch off the options that clash with FusionFix"
  fi
fi

# --- 6. FusionFix ---------------------------------------------------------------------------
if (( FUSIONFIX )); then
  step "FusionFix"
  if need_asset GTAIV.EFLC.FusionFix.zip; then
    # plugins/ (the .asi + its .ini) and update/ (replacement data, loaded by FusionFix).
    # Not dinput8.dll: that is the CE ASI loader; 1.0.8.0 uses xlive.dll (step 4).
    pats=('plugins/*' 'update/*')
    (( DXVK )) && pats+=(d3d9.dll vulkan.dll)
    unpack "$LC_CACHE/GTAIV.EFLC.FusionFix.zip" "$STAGE/ff" "${pats[@]}"
    ok "FusionFix $(pe_version "$STAGE/ff/plugins/GTAIV.EFLC.FusionFix.asi")"
    install_tree "$STAGE/ff" "" keep-ini
    (( DXVK )) || dim "d3d9.dll/vulkan.dll not installed (Proton already translates D3D9 via DXVK; use --dxvk to add them)"
  else
    plan "extract plugins/ and update/ from GTAIV.EFLC.FusionFix.zip (after downloading it)"
  fi
fi

# --- 7. LibertyCraft.asi -------------------------------------------------------------------
step "LibertyCraft plugin"
install_lc_plugin || info "dist/plugins/LibertyCraft.asi not built yet - run tools/build.sh (it can install it with --install)"

# Files the mods write on first start; recorded so --uninstall cleans them up too.
for f in d3d9.cfg plugins/GTAIV.EFLC.FusionFix.cfg LibertyCraft.log; do track_runtime_file "$f"; done

# --- 8. verify -------------------------------------------------------------------------------
step "Checking the game folder"
if (( DRY_RUN )); then
  plan "verify GTAIV.exe is $TARGET_VERSION, loader + plugins present, no dinput8.dll"
else
  bad=0
  check() { if eval "$2"; then ok "$1"; else err "$1"; bad=1; fi; }
  v=$(pe_version "$GAME/GTAIV.exe")
  check "GTAIV.exe version $v" '[[ $v == "$TARGET_VERSION" ]]'
  check "xlive.dll (ASI loader)" '[[ -f $GAME/xlive.dll ]]'
  check "no XLivelessAddon" '[[ ! -e $GAME/plugins/XLivelessAddon.asi ]]'
  check "no dinput8.dll" '[[ ! -e $GAME/dinput8.dll ]]'
  if (( ZOLIKA )); then check "ZolikaPatch.asi + .ini" '[[ -f $GAME/ZolikaPatch.asi && -f $GAME/ZolikaPatch.ini ]]'; fi
  if (( FUSIONFIX )); then
    check "plugins/GTAIV.EFLC.FusionFix.asi" '[[ -f $GAME/plugins/GTAIV.EFLC.FusionFix.asi ]]'
    check "update/ folder" '[[ -d $GAME/update ]]'
  fi
  check "backup manifest ($(manifest_count) entries)" '[[ -f $(manifest_path) ]]'
  (( bad )) && die "The game folder is not in the expected state (see above)."
fi

# FusionFix's README asks Proton users for Microsoft's d3dx9_43.dll. Steam normally installs
# it from the DirectX redist on first launch; Proton's builtin replacement is a Wine stub.
if PFX=$(find_proton_prefix); then
  d3dx="$PFX/drive_c/windows/syswow64/d3dx9_43.dll"
  if [[ -f $d3dx ]] && ! LC_ALL=C grep -qaF 'Wine builtin DLL' "$d3dx"; then
    ok "Proton prefix has Microsoft's d3dx9_43.dll"
  else
    warn "Proton prefix lacks Microsoft's d3dx9_43.dll (FusionFix recommends it). Fix:"
    warn "  protontricks $LC_STEAM_APPID d3dx9_43     (paru -S protontricks)"
  fi
else
  info "No Proton prefix yet - start the game once from Steam so it gets created."
fi

# --- 9. Minecraft --------------------------------------------------------------------------
if (( MINECRAFT )); then
  step "Minecraft: Prism Launcher instance \"$LC_INSTANCE_NAME\""
  if ! find_prism_dir >/dev/null; then
    warn "Prism Launcher data folder not found. Install and start Prism once (paru -S prismlauncher),"
    warn "then run: tools/install.sh --no-fusionfix --no-zolikapatch   (re-running is safe)"
  else
    if prism_running; then
      warn "Prism Launcher is running; it may overwrite instance files when it exits."
      confirm "Continue anyway? (better: quit Prism and re-run)" || die "Aborted."
    fi
    INST=$(lc_instance_dir)
    if [[ -d $INST ]]; then info "updating $INST (worlds and options are not touched)"; else info "creating $INST"; fi
    [[ -d "$INST/minecraft/mods" ]] || run mkdir -p -- "$INST/minecraft/mods"

    # instance.cfg: keys LibertyCraft depends on are always enforced; the rest are only
    # defaults for a new instance (the user may tune memory, console, ... in Prism).
    enforce=(InstanceType=OneSix OverrideJavaArgs=true "JvmArgs=$JVM_ARGS" OverrideMemory=true)
    if JHOME=$(find_java25_home); then
      enforce+=(OverrideJavaLocation=true "JavaPath=$JHOME/bin/java")
      ok "Java 25: $JHOME"
    else
      warn "No Java 25 found; Prism will pick/download one (Minecraft $MC_VERSION needs Java 25)."
    fi
    defaults=(ConfigVersion=1.3 "name=$LC_INSTANCE_NAME" iconKey=default
      "notes=Managed by LibertyCraft (tools/install.sh). Started by tools/launch.sh; runs hidden while GTA IV shows the game."
      MinMemAlloc=1024 MaxMemAlloc=4096 OverrideConsole=true ShowConsole=false AutoCloseConsole=false ShowConsoleOnError=true)
    if (( DRY_RUN )); then
      plan "instance.cfg [General]: ${enforce[*]}"
      plan "instance.cfg defaults (if unset): ${defaults[*]}"
    else
      res=$(INI_ADD=1 ini_set "$INST/instance.cfg" General "${enforce[@]}")
      res2=$(INI_ADD=missing ini_set "$INST/instance.cfg" General "${defaults[@]}")
      ok "instance.cfg (enforced: ${res%%, missing*}; defaults: ${res2%%, missing*})"
    fi

    # mmc-pack.json: Minecraft + Fabric versions. Prism resolves libraries from these uids.
    cat > "$STAGE/mmc-pack.json" <<EOF
{
    "components": [
        {
            "cachedName": "LWJGL 3",
            "cachedVersion": "$LWJGL_VERSION",
            "cachedVolatile": true,
            "dependencyOnly": true,
            "uid": "org.lwjgl3",
            "version": "$LWJGL_VERSION"
        },
        {
            "cachedName": "Minecraft",
            "cachedRequires": [ { "suggests": "$LWJGL_VERSION", "uid": "org.lwjgl3" } ],
            "cachedVersion": "$MC_VERSION",
            "important": true,
            "uid": "net.minecraft",
            "version": "$MC_VERSION"
        },
        {
            "cachedName": "Intermediary Mappings",
            "cachedRequires": [ { "equals": "$MC_VERSION", "uid": "net.minecraft" } ],
            "cachedVersion": "$MC_VERSION",
            "cachedVolatile": true,
            "dependencyOnly": true,
            "uid": "net.fabricmc.intermediary",
            "version": "$MC_VERSION"
        },
        {
            "cachedName": "Fabric Loader",
            "cachedRequires": [ { "uid": "net.fabricmc.intermediary" } ],
            "cachedVersion": "$LOADER_VERSION",
            "uid": "net.fabricmc.fabric-loader",
            "version": "$LOADER_VERSION"
        }
    ],
    "formatVersion": 1
}
EOF
    pack="$INST/mmc-pack.json"
    if [[ -f $pack ]] && cmp -s "$STAGE/mmc-pack.json" "$pack"; then
      ok "mmc-pack.json (unchanged)"
    else
      if [[ -f $pack && ! -f $pack.libertycraft-bak ]]; then run cp -p -- "$pack" "$pack.libertycraft-bak"; fi
      run cp -f -- "$STAGE/mmc-pack.json" "$pack"
      ok "mmc-pack.json: Minecraft $MC_VERSION, Fabric Loader $LOADER_VERSION, LWJGL $LWJGL_VERSION"
    fi

    # Fabric API (pinned) and, if built, the LibertyCraft mod.
    rc=0; fetch "$FABRIC_API_URL" "$LC_CACHE/$FABRIC_API_JAR" "$FABRIC_API_SUM" || rc=$?
    if (( rc == 0 )); then install_mod_jar "$LC_CACHE/$FABRIC_API_JAR" "$INST/minecraft/mods" 'fabric-api-*.jar'
    elif (( rc == 2 )); then plan "copy $FABRIC_API_JAR into mods/"
    else die "Could not get Fabric API"; fi
    if JAR=$(latest_lc_jar); then install_mod_jar "$JAR" "$INST/minecraft/mods" 'libertycraft-*.jar'
    else info "dist/mods/libertycraft-*.jar not built yet - run tools/build.sh --fabric"; fi
  fi
fi

# --- done ----------------------------------------------------------------------------------
step "Done"
if (( DRY_RUN )); then info "That was a dry run; nothing was changed. Run without --dry-run to apply."; fi
cat <<EOF
    Steam -> GTA IV -> Properties -> General -> Launch options (recommended):
      ${C_BOLD}PROTON_LOG=1 %command% -availablevidmem 8192 -nomemrestrict -norestrictions${C_RESET}
    PROTON_LOG=1 writes ~/steam-$LC_STEAM_APPID.log (useful while developing; drop it later).
    WINEDLLOVERRIDES="dinput8=n,b" is no longer needed (the loader is xlive.dll now) but is harmless.
    Steam updates / "Verify integrity of game files" put the Complete Edition back; re-run this
    script afterwards. To undo LibertyCraft's changes: tools/install.sh --uninstall
    Next: tools/build.sh --install, then tools/launch.sh
EOF
