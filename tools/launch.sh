#!/usr/bin/env bash
# tools/launch.sh - start Minecraft (hidden, via Prism) and then GTA IV (via Steam).
#
# Minecraft goes first and GTA IV only starts once Minecraft has created and hidden its window:
# a window appearing while GTA IV loads takes the focus, and GTA IV stops loading without it.
# (Minecraft ignores a stale bridge header from an earlier GTA IV run, so this order is safe.)
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

if (( MC )); then
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
  MCLOG="$INST/minecraft/logs/latest.log"
  if pgrep -f '[-]Dlibertycraft\.startHidden' >/dev/null 2>&1; then
    ok "Minecraft is already running"
  elif (( DRY_RUN )); then
    plan "$PRISM --launch $LC_INSTANCE_NAME &"
    (( GTA )) && plan "wait until Minecraft has hidden its window (latest.log), then start GTA IV"
  else
    started=$(date +%s)
    nohup $PRISM --launch "$LC_INSTANCE_NAME" >/dev/null 2>&1 &
    disown
    ok "started (Minecraft runs hidden: -Dlibertycraft.startHidden=true)"
    # Wait for Minecraft to create *and hide* its window before GTA IV starts: a window popping up
    # takes the focus, and GTA IV stops loading while it isn't the focused window (seen in testing).
    if (( GTA )); then
      info "waiting for Minecraft to finish starting (up to 3 minutes)..."
      ready=0
      for _ in $(seq 1 180); do
        if [[ -f $MCLOG && $(stat -c %Y "$MCLOG") -ge $started ]] && grep -q 'game window hidden' "$MCLOG"; then
          ready=1; break
        fi
        sleep 1
      done
      if (( ready )); then ok "Minecraft is up ($(( $(date +%s) - started )) s)"
      else warn "Minecraft didn't report its hidden window in time; starting GTA IV anyway"; fi
      sleep 2   # let the focus settle
    fi
  fi
fi

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

step "Logs"
[[ -n $INST ]] && info "Minecraft:   $INST/minecraft/logs/latest.log"
[[ -n $GAME ]] && info "LibertyCraft: $GAME/LibertyCraft.log"
info "Proton:      ~/steam-$LC_STEAM_APPID.log  (only with PROTON_LOG=1 in Steam's launch options)"
