#!/usr/bin/env bash
# tools/dev/run-gta-with-log.sh - start GTA IV through Proton *directly* (not via the Steam
# client) with PROTON_LOG=1, so a crash leaves a log without touching Steam's launch options.
# Steam must be running (the game talks to it through steam_api.dll).
#
#   tools/dev/run-gta-with-log.sh [extra WINEDEBUG channels, e.g. +loaddll]
# Log: ~/steam-12210.log (or $PROTON_LOG_DIR/steam-12210.log)
set -euo pipefail
# shellcheck source=../lib/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

GAME=$(find_gta_dir) || die "GTA IV not found (set GTAIV_DIR)"
LIB=$(steam_library_for_app) || die "Steam library for $LC_STEAM_APPID not found"
COMPAT="$LIB/steamapps/compatdata/$LC_STEAM_APPID"
[[ -d $COMPAT/pfx ]] || die "No Proton prefix yet - start the game once from Steam"
# The Proton build the prefix was last run with (first line of config_info, e.g. "11.0-100").
PROTON=""
want=$(head -n1 "$COMPAT/config_info" 2>/dev/null | cut -d- -f1)
for d in "$LIB/steamapps/common/Proton $want" "$LIB/steamapps/common/Proton - Experimental" "$LIB"/steamapps/common/Proton*; do
  [[ -x "$d/proton" ]] && { PROTON="$d/proton"; break; }
done
[[ -n $PROTON ]] || die "No Proton install found under $LIB/steamapps/common"
STEAM_ROOT=$(steam_roots | head -n1)
gta_running && die "GTA IV is already running"
pgrep -x steam >/dev/null 2>&1 || warn "Steam does not seem to be running; the game may refuse to start"

dbg=${1:-}
info "Proton: $PROTON"
info "log:    ${PROTON_LOG_DIR:-$HOME}/steam-$LC_STEAM_APPID.log"
cd "$GAME"
env STEAM_COMPAT_DATA_PATH="$COMPAT" STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT" \
    SteamAppId=$LC_STEAM_APPID SteamGameId=$LC_STEAM_APPID PROTON_LOG=1 \
    ${dbg:+"WINEDEBUG=+timestamp,+pid,+tid,+seh,+debugstr,+module,$dbg"} \
  "$PROTON" waitforexitandrun "$GAME/GTAIV.exe" "${@:2}"
