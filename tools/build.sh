#!/usr/bin/env bash
# tools/build.sh: build LibertyCraft.asi (GTA IV plugin) and the Fabric mod into dist/.
#
#   dist/plugins/LibertyCraft.asi (+ .ini/.pdb)   <- asi/   (clang-cl + xwin, see setup-toolchain.sh)
#   dist/mods/libertycraft-<version>.jar          <- fabric/ (Gradle, Java 25)
#
# --install then copies them into the game folder and the Prism instance, using the same
# tracked/backed-up copy as tools/install.sh (so --uninstall removes the plugin again).
set -euo pipefail
# shellcheck source=lib/gamefiles.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/gamefiles.sh"

usage() {
  cat <<USAGE
Usage: tools/build.sh [--asi] [--fabric] [--install] [--debug] [-n|--dry-run] [-y|--yes]
  --asi       build only the GTA IV plugin      (default: both)
  --fabric    build only the Minecraft mod      (default: both)
  --debug     use the asi "debug" CMake preset instead of "release"
  --install   copy the results into <gamedir>/plugins/ and the Prism instance's mods/
USAGE
}
DO_ASI=0 DO_FABRIC=0 INSTALL=0 PRESET=release
while (( $# )); do
  case $1 in
    --asi)        DO_ASI=1 ;;
    --fabric)     DO_FABRIC=1 ;;
    --install)    INSTALL=1 ;;
    --debug)      PRESET=debug ;;
    -n|--dry-run) DRY_RUN=1 ;;
    -y|--yes)     ASSUME_YES=1 ;;
    -h|--help)    usage; exit 0 ;;
    *) usage >&2; die "Unknown option: $1" ;;
  esac
  shift
done
(( DO_ASI || DO_FABRIC )) || { DO_ASI=1; DO_FABRIC=1; }

if (( DO_ASI )); then
  step "LibertyCraft.asi (cmake --preset $PRESET)"
  require_tools cmake:cmake ninja:ninja clang-cl:clang lld-link:lld
  [[ -d "$LC_ROOT/.tools/xwin/crt/include" ]] || die "Windows SDK/CRT missing: run tools/setup-toolchain.sh first"
  [[ -f "$LC_ROOT/asi/CMakePresets.json" ]] || die "asi/CMakePresets.json not found"
  # Presets resolve relative to the source dir, so run cmake from asi/.
  (cd "$LC_ROOT/asi" && run cmake --preset "$PRESET" && run cmake --build --preset "$PRESET")
  out="$LC_ROOT/asi/build/$PRESET"
  if (( ! DRY_RUN )); then
    [[ -f "$out/LibertyCraft.asi" ]] || die "build finished but $out/LibertyCraft.asi is missing"
  fi
  run mkdir -p -- "$LC_DIST/plugins"
  run cp -f -- "$out/LibertyCraft.asi" "$LC_DIST/plugins/"
  [[ -f "$out/LibertyCraft.pdb" ]] && run cp -f -- "$out/LibertyCraft.pdb" "$LC_DIST/plugins/"
  for ini in "$LC_ROOT/asi/dist/LibertyCraft.ini" "$out/LibertyCraft.ini"; do
    if [[ -f $ini ]]; then run cp -f -- "$ini" "$LC_DIST/plugins/"; break; fi
  done
  ok "dist/plugins/LibertyCraft.asi"
fi

if (( DO_FABRIC )); then
  step "Fabric mod (gradlew build)"
  if JHOME=$(find_java25_home); then ok "Java 25: $JHOME"
  else
    command -v java >/dev/null 2>&1 || die "No Java found. Install a JDK 25 (paru -S jdk-openjdk) or launch MC 26.3 in Prism once."
    JHOME=$(dirname "$(dirname "$(realpath "$(command -v java)")")")
    warn "No Java 25 found; trying $JHOME (Minecraft 26.3 needs 25, this will probably fail)"
  fi
  (cd "$LC_ROOT/fabric" && run env JAVA_HOME="$JHOME" ./gradlew build --no-daemon)
  run mkdir -p -- "$LC_DIST/mods"
  n=0
  for jar in "$LC_ROOT"/fabric/build/libs/libertycraft-*.jar; do
    [[ -f $jar ]] || continue
    [[ $jar == *-sources.jar || $jar == *-dev.jar ]] && continue
    # keep a single version in dist/mods so install/launch pick the right one
    for old in "$LC_DIST"/mods/libertycraft-*.jar; do
      [[ -f $old && ${old##*/} != "${jar##*/}" ]] && run rm -f -- "$old"
    done
    run cp -f -- "$jar" "$LC_DIST/mods/"
    ok "dist/mods/${jar##*/}"; n=$((n + 1))
  done
  (( n || DRY_RUN )) || die "no fabric/build/libs/libertycraft-*.jar produced"
fi

if (( INSTALL )); then
  if (( DO_ASI )); then
    step "Installing LibertyCraft.asi into the game"
    GAME=$(find_gta_dir) || die "GTA IV not found (set GTAIV_DIR)"
    gta_running && die "GTA IV is running; quit it first (it only loads plugins at startup)."
    [[ $(pe_version "$GAME/GTAIV.exe") == 1.0.8.0 ]] || warn "GTAIV.exe is not 1.0.8.0 - run tools/install.sh first"
    manifest_load "$GAME"
    install_lc_plugin || { (( DRY_RUN )) && plan "copy dist/plugins/LibertyCraft.* to $GAME/plugins/" || die "nothing in dist/plugins"; }
  fi
  if (( DO_FABRIC )); then
    step "Installing the mod into the Prism instance"
    INST=$(lc_instance_dir) || die "Prism Launcher not found"
    [[ -d $INST ]] || die "Prism instance $INST missing - run tools/install.sh first"
    if JAR=$(latest_lc_jar); then install_mod_jar "$JAR" "$INST/minecraft/mods" 'libertycraft-*.jar'
    elif (( DRY_RUN )); then plan "copy dist/mods/libertycraft-*.jar to $INST/minecraft/mods/"
    else die "nothing in dist/mods"; fi
  fi
fi
step "Done"
