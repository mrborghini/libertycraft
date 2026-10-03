#!/usr/bin/env bash
# tools/setup-toolchain.sh: check the build tools and fetch the Windows SDK/CRT headers + libs.
#
# LibertyCraft.asi is a 32-bit Windows DLL, cross-compiled on Linux with clang-cl + lld-link.
# The MSVC CRT and Windows SDK it links against are "splatted" by xwin into <repo>/.tools/xwin
# (Microsoft's license must be accepted; xwin downloads from Microsoft's servers, ~1 GB cache).
# Nothing is installed system-wide: missing tools are reported with the Arch package to install.
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() {
  cat <<USAGE
Usage: tools/setup-toolchain.sh [-n|--dry-run] [-y|--yes] [-h|--help]
  Checks: xwin clang-cl lld-link llvm-rc cmake ninja jq 7z unzip curl python3 java
  Then splats the MSVC CRT + Windows SDK (x86 and x86_64) into .tools/xwin if missing.
USAGE
}
while (( $# )); do
  case $1 in
    -n|--dry-run) DRY_RUN=1 ;;
    -y|--yes)     ASSUME_YES=1 ;;
    -h|--help)    usage; exit 0 ;;
    *) usage >&2; die "Unknown option: $1" ;;
  esac
  shift
done

XWIN_OUT="$LC_ROOT/.tools/xwin"
XWIN_LOG="$LC_ROOT/.tools/xwin-splat.log"
XWIN_CACHE="${XWIN_CACHE:-$HOME/.xwin-cache}"

step "Build tools"
tools_ok=1
check_tools xwin:xwin clang-cl:clang lld-link:lld llvm-rc:llvm cmake:cmake ninja:ninja jq:jq \
            7z:7zip unzip:unzip curl:curl python3:python java:jdk-openjdk || tools_ok=0
if J=$(find_java25_home); then ok "Java 25 for Gradle: $J"
else warn "No Java 25 found (Minecraft 26.3 mods need it). Prism downloads one when you first launch 26.3, or: paru -S jdk-openjdk"; fi

step "Windows SDK + MSVC CRT (xwin splat -> ${XWIN_OUT#"$LC_ROOT"/})"
if [[ -d "$XWIN_OUT/crt/include" && -d "$XWIN_OUT/sdk/include" ]]; then
  ok "already present"
elif [[ -f $XWIN_LOG ]] && ! grep -q '^exit=' "$XWIN_LOG"; then
  # A splat started earlier is still running (it appends "exit=<code>" when it finishes).
  warn "splat in progress (see ${XWIN_LOG#"$LC_ROOT"/}); not starting a second one"
elif ! command -v xwin >/dev/null 2>&1; then
  err "xwin missing - install it first (paru -S xwin)"; tools_ok=0
else
  info "This downloads the MSVC CRT + Windows SDK from Microsoft (several hundred MB into $XWIN_CACHE)"
  info "and means accepting Microsoft's license: https://go.microsoft.com/fwlink/?LinkId=2086102"
  if confirm "Accept the license and download?"; then
    if (( DRY_RUN )); then
      plan "xwin --accept-license --cache-dir $XWIN_CACHE --arch x86 --arch x86_64 splat --output $XWIN_OUT"
    else
      mkdir -p -- "$(dirname "$XWIN_OUT")"
      rc=0
      xwin --accept-license --cache-dir "$XWIN_CACHE" --arch x86 --arch x86_64 splat --output "$XWIN_OUT" \
        2>&1 | tee "$XWIN_LOG" || rc=$?
      echo "exit=$rc" >> "$XWIN_LOG"
      (( rc == 0 )) || die "xwin splat failed (exit $rc), log: $XWIN_LOG"
      ok "splatted into $XWIN_OUT"
    fi
  else
    warn "skipped; LibertyCraft.asi cannot be built without it"; tools_ok=0
  fi
fi

step "Done"
if (( tools_ok )); then ok "toolchain ready - next: tools/build.sh"
else die "toolchain incomplete (see above)"; fi
