#!/usr/bin/env bash
# tools/launch.sh - start Minecraft (hidden, via Prism) and then GTA IV (via Steam).
#
# Minecraft goes first: it owns the world and opens the shared-memory file that
# LibertyCraft.asi connects to; the plugin retries, so the order is not critical.
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() {
  cat <<USAGE
Usage: tools/launch.sh [--minecraft-only | --gta-only] [-n|--dry-run]
USAGE
}
MC=1 GTA=1
while (( $# )); do
  case $1 in
    --minecraft-only) GTA=0 ;;
    --gta-only)       MC=0 ;;
    -n|--dry-run)     DRY_RUN=1 ;;
    -h|--help)        usage; exit 0 ;;
    *) usage >&2; die "Unknown option: $1" ;;
  esac
  shift
done
(( MC || GTA )) || die "--minecraft-only and --gta-only exclude each other"

INST=$(lc_instance_dir 2>/dev/null || true)
GAME=$(find_gta_dir 2>/dev/null || true)

if (( GTA )); then
  step "GTA IV (Steam app $LC_STEAM_APPID)"
  if [[ -n $GAME ]]; then
    v=$(pe_version "$GAME/GTAIV.exe")
    [[ $v == 1.0.8.0 ]] || warn "GTAIV.exe is $v, not 1.0.8.0 (Steam update/verify?) - run tools/install.sh"
    [[ -f "$GAME/plugins/LibertyCraft.asi" ]] || warn "plugins/LibertyCraft.asi missing - run tools/build.sh --asi --install"
  fi
  if gta_running; then warn "GTA IV is already running"
  else
    url="steam://rungameid/$LC_STEAM_APPID"
    if command -v steam >/dev/null 2>&1; then
      if (( DRY_RUN )); then plan "steam $url"; else nohup steam "$url" >/dev/null 2>&1 & disown; fi
    else
      run xdg-open "$url"
    fi
    ok "asked Steam to start the game (launch options set in Steam apply)"
  fi
fi

if (( MC )); then
  # GTA IV first: it (re)writes the bridge header, so Minecraft finds a live host instead of a
  # stale one from an earlier run. Both load in parallel anyway; GTA's intro takes longer.
  (( GTA )) && { if (( DRY_RUN )); then plan "sleep 3"; else sleep 3; fi; }
  step "Minecraft (Prism instance $LC_INSTANCE_NAME)"
  [[ -n $INST && -d $INST ]] || die "Prism instance not found - run tools/install.sh"
  compgen -G "$INST/minecraft/mods/libertycraft-*.jar" >/dev/null \
    || warn "no libertycraft-*.jar in the instance's mods/ - run tools/build.sh --fabric --install"
  PRISM=""
  for c in prismlauncher org.prismlauncher.PrismLauncher; do
    command -v "$c" >/dev/null 2>&1 && { PRISM=$c; break; }
  done
  if [[ -z $PRISM ]] && command -v flatpak >/dev/null 2>&1 && flatpak info org.prismlauncher.PrismLauncher >/dev/null 2>&1; then
    PRISM="flatpak run org.prismlauncher.PrismLauncher"
  fi
  [[ -n $PRISM ]] || die "Prism Launcher not found (paru -S prismlauncher)"
  # -l/--launch starts the instance directly (and hands over to an already running Prism).
  # shellcheck disable=SC2086
  if (( DRY_RUN )); then plan "$PRISM --launch $LC_INSTANCE_NAME &"
  else
    nohup $PRISM --launch "$LC_INSTANCE_NAME" >/dev/null 2>&1 &
    disown
    ok "started (Minecraft runs hidden: -Dlibertycraft.startHidden=true)"
  fi
fi

step "Logs"
[[ -n $INST ]] && info "Minecraft:   $INST/minecraft/logs/latest.log"
[[ -n $GAME ]] && info "LibertyCraft: $GAME/LibertyCraft.log"
info "Proton:      ~/steam-$LC_STEAM_APPID.log  (only with PROTON_LOG=1 in Steam's launch options)"
