#!/usr/bin/env bash
# tools/lib/common.sh - helpers shared by the LibertyCraft scripts. Source it, do not run it.
#
#   source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"
#
# Every function here is side-effect free unless its name says otherwise, and the
# ones that do touch the filesystem honour DRY_RUN=1 (set by the --dry-run flag).

[[ -n "${_LC_COMMON_LOADED:-}" ]] && return 0
_LC_COMMON_LOADED=1

LC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LC_TOOLS="$LC_ROOT/tools"
LC_CACHE="${LIBERTYCRAFT_CACHE:-$LC_TOOLS/cache}"   # downloaded zips (gitignored)
LC_DIST="$LC_ROOT/dist"                              # build outputs (gitignored)
LC_INSTANCE_NAME="LibertyCraft"                      # Prism instance id == folder name
LC_STEAM_APPID=12210                                 # GTA IV: The Complete Edition

DRY_RUN=${DRY_RUN:-0}
ASSUME_YES=${ASSUME_YES:-0}

# ---------------------------------------------------------------------------
# Output. Colours only when stdout is a terminal (and NO_COLOR is unset).
# ---------------------------------------------------------------------------
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  C_RESET=$'\e[0m'; C_BOLD=$'\e[1m'; C_DIM=$'\e[2m'
  C_RED=$'\e[31m'; C_GREEN=$'\e[32m'; C_YELLOW=$'\e[33m'; C_BLUE=$'\e[34m'; C_CYAN=$'\e[36m'
else
  C_RESET=''; C_BOLD=''; C_DIM=''; C_RED=''; C_GREEN=''; C_YELLOW=''; C_BLUE=''; C_CYAN=''
fi

_LC_STEP=0
step() { _LC_STEP=$((_LC_STEP + 1)); printf '\n%s==> [%d] %s%s\n' "${C_BOLD}${C_BLUE}" "$_LC_STEP" "$*" "$C_RESET"; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '    %s+%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn() { printf '    %s! %s%s\n' "$C_YELLOW" "$*" "$C_RESET" >&2; }
err()  { printf '    %sx %s%s\n' "$C_RED" "$*" "$C_RESET" >&2; }
die()  { err "$@"; exit 1; }
plan() { printf '    %s[dry-run]%s %s\n' "$C_CYAN" "$C_RESET" "$*"; }   # what we *would* do
dim()  { printf '    %s%s%s\n' "$C_DIM" "$*" "$C_RESET"; }

# run CMD... : execute, or print it when DRY_RUN=1.
run() {
  if (( DRY_RUN )); then plan "$(printf '%q ' "$@")"; else "$@"; fi
}

# confirm "question" : 0 = yes. --yes answers yes; --dry-run never blocks;
# a non-interactive shell without --yes aborts (never silently proceed).
confirm() {
  if (( ASSUME_YES )); then dim "$1 -> yes (--yes)"; return 0; fi
  if (( DRY_RUN ));    then plan "would ask: $1"; return 0; fi
  [[ -t 0 ]] || die "Not interactive and --yes not given. Question was: $1"
  local ans
  read -r -p "    $1 [y/N] " ans
  [[ $ans =~ ^[Yy]([Ee][Ss])?$ ]]
}

# ---------------------------------------------------------------------------
# Tools. We never install anything ourselves; we print the Arch command instead.
# Usage:  check_tools unzip:unzip curl:curl python3:python 7z:7zip
#         require_tools ...   (same, but dies when something is missing)
# ---------------------------------------------------------------------------
_LC_MISSING_PKGS=()
check_tools() {
  local spec cmd pkg
  _LC_MISSING_PKGS=()
  for spec in "$@"; do
    cmd=${spec%%:*}; pkg=${spec#*:}
    if command -v "$cmd" >/dev/null 2>&1; then
      ok "$cmd ($(command -v "$cmd"))"
    else
      err "$cmd not found"
      _LC_MISSING_PKGS+=("$pkg")
    fi
  done
  if (( ${#_LC_MISSING_PKGS[@]} )); then
    warn "Install the missing tools, e.g. on Arch:  paru -S ${_LC_MISSING_PKGS[*]}"
    return 1
  fi
  return 0
}
require_tools() { check_tools "$@" || die "Missing required tools (see above)."; }

# ---------------------------------------------------------------------------
# Processes. Patterns use the [b]racket trick so pgrep never matches the shell
# that is running our own script (its command line contains the pattern text).
# ---------------------------------------------------------------------------
GTA_PROC_RE='[G]TAIV\.exe|[P]layGTAIV\.exe'
gta_running()   { pgrep -f "$GTA_PROC_RE" >/dev/null 2>&1; }
prism_running() { pgrep -x prismlauncher >/dev/null 2>&1 || pgrep -f '[p]rismlauncher' >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# Steam. We only *read* Steam's files (libraryfolders.vdf, appmanifest_*.acf);
# we never edit them.
# ---------------------------------------------------------------------------
steam_roots() {
  local -a cands=("${STEAM_ROOT:-}" "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root"
                  "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam")
  local c r
  declare -A seen=()
  for c in "${cands[@]}"; do
    [[ -n $c && -d $c ]] || continue
    r=$(realpath "$c")
    [[ -n ${seen[$r]:-} ]] && continue
    seen[$r]=1
    printf '%s\n' "$r"
  done
}

# All Steam library folders (the client root plus every "path" in libraryfolders.vdf).
steam_libraries() {
  local root vdf p r
  declare -A seen=()
  while IFS= read -r root; do
    for p in "$root" $(sed -nE 's/^[[:space:]]*"path"[[:space:]]+"([^"]+)".*$/\1/p' "$root/steamapps/libraryfolders.vdf" 2>/dev/null | tr ' ' '\001'); do
      p=${p//$'\001'/ }
      [[ -d $p ]] || continue
      r=$(realpath "$p")
      [[ -n ${seen[$r]:-} ]] && continue
      seen[$r]=1
      printf '%s\n' "$r"
    done
  done < <(steam_roots)
}

# Library folder that holds appmanifest_<appid>.acf.
steam_library_for_app() {
  local appid=${1:-$LC_STEAM_APPID} lib
  while IFS= read -r lib; do
    [[ -f "$lib/steamapps/appmanifest_$appid.acf" ]] && { printf '%s\n' "$lib"; return 0; }
  done < <(steam_libraries)
  return 1
}

# Directory that contains GTAIV.exe. Override with GTAIV_DIR.
# Steam's install dir is ".../common/Grand Theft Auto IV/", the exe lives one level down in GTAIV/.
find_gta_dir() {
  if [[ -n ${GTAIV_DIR:-} ]]; then
    [[ -f "$GTAIV_DIR/GTAIV.exe" ]] || die "GTAIV_DIR=$GTAIV_DIR does not contain GTAIV.exe"
    printf '%s\n' "${GTAIV_DIR%/}"; return 0
  fi
  local lib acf installdir dir
  lib=$(steam_library_for_app "$LC_STEAM_APPID") || return 1
  acf="$lib/steamapps/appmanifest_$LC_STEAM_APPID.acf"
  installdir=$(sed -nE 's/^[[:space:]]*"installdir"[[:space:]]+"([^"]+)".*$/\1/p' "$acf" | head -n1)
  [[ -n $installdir ]] || return 1
  for dir in "$lib/steamapps/common/$installdir/GTAIV" "$lib/steamapps/common/$installdir"; do
    [[ -f "$dir/GTAIV.exe" ]] && { printf '%s\n' "$dir"; return 0; }
  done
  return 1
}

# Proton prefix for the game (compatdata/<appid>/pfx). Override with GTAIV_PREFIX.
find_proton_prefix() {
  if [[ -n ${GTAIV_PREFIX:-} ]]; then printf '%s\n' "$GTAIV_PREFIX"; return 0; fi
  local lib
  lib=$(steam_library_for_app "$LC_STEAM_APPID") || return 1
  [[ -d "$lib/steamapps/compatdata/$LC_STEAM_APPID/pfx" ]] || return 1
  printf '%s\n' "$lib/steamapps/compatdata/$LC_STEAM_APPID/pfx"
}

# Version string from a PE file's VS_VERSION_INFO resource, e.g. 1.0.8.0.
pe_version() {
  python3 - "$1" <<'PY'
import struct, sys
try:
    d = open(sys.argv[1], 'rb').read()
    i = d.find('VS_VERSION_INFO'.encode('utf-16-le'))
    j = d.find(b'\xbd\x04\xef\xfe', i) if i >= 0 else -1   # VS_FIXEDFILEINFO signature
    if j < 0:
        print('unknown'); sys.exit(0)
    ms, ls = struct.unpack_from('<II', d, j + 8)
    print(f"{ms >> 16}.{ms & 0xffff}.{ls >> 16}.{ls & 0xffff}")
except Exception as e:  # noqa: BLE001 - a bad exe must not kill the installer
    print('unknown')
PY
}

# ---------------------------------------------------------------------------
# Prism Launcher / Java
# ---------------------------------------------------------------------------
find_prism_dir() {
  local d
  for d in "${PRISM_DIR:-}" "$HOME/.local/share/PrismLauncher" \
           "$HOME/.var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher"; do
    [[ -n $d && -d $d ]] && { printf '%s\n' "$d"; return 0; }
  done
  return 1
}

# Prism's instance folder (honours InstanceDir= in prismlauncher.cfg).
prism_instances_dir() {
  local prism inst
  prism=$(find_prism_dir) || return 1
  inst=$(sed -nE 's/^InstanceDir=(.*)$/\1/p' "$prism/prismlauncher.cfg" 2>/dev/null | head -n1)
  inst=${inst:-instances}
  [[ $inst == /* ]] || inst="$prism/$inst"
  printf '%s\n' "$inst"
}

lc_instance_dir() { local d; d=$(prism_instances_dir) || return 1; printf '%s/%s\n' "$d" "$LC_INSTANCE_NAME"; }

# A Java 25 home. Prism-managed runtimes first (~/.local/share/PrismLauncher/java/*/release),
# then system JDKs, then whatever `java` is on PATH if it is new enough. Override: JAVA25_HOME.
find_java25_home() {
  if [[ -n ${JAVA25_HOME:-} ]]; then printf '%s\n' "$JAVA25_HOME"; return 0; fi
  local prism rel d v
  if prism=$(find_prism_dir); then
    for rel in "$prism"/java/*/release; do
      [[ -f $rel ]] || continue
      if grep -q '^JAVA_VERSION="25' "$rel" 2>/dev/null; then dirname "$rel"; return 0; fi
    done
  fi
  for d in /usr/lib/jvm/java-25-openjdk /usr/lib/jvm/java-25*; do
    [[ -x "$d/bin/java" ]] && { printf '%s\n' "$d"; return 0; }
  done
  if command -v java >/dev/null 2>&1; then
    v=$(java -version 2>&1 | sed -nE 's/.*version "([0-9]+).*/\1/p' | head -n1)
    if [[ -n $v ]] && (( v >= 25 )); then
      dirname "$(dirname "$(realpath "$(command -v java)")")"; return 0
    fi
  fi
  return 1
}

# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------
sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

# ini_set FILE SECTION KEY=VALUE... : edit KEY=VALUE lines inside [SECTION] in place.
# Preserves comments, ordering and CRLF line endings. INI_ADD selects the mode:
#   0 (default) only change keys that already exist   (ZolikaPatch.ini: never invent keys)
#   1           change existing keys, append missing ones (and the section if needed)
#   missing     only append missing keys, never change existing ones (defaults for instance.cfg)
# The file is only rewritten when something actually changes.
# Prints "<n> changed, <m> added, missing: ..." on stdout.
ini_set() {
  INI_ADD="${INI_ADD:-0}" python3 - "$@" <<'PY'
import os, re, sys
path, section, *pairs = sys.argv[1:]
mode = os.environ.get('INI_ADD', '0')
add_missing = mode in ('1', 'missing')
change_existing = mode != 'missing'
want = {}
for p in pairs:
    k, _, v = p.partition('=')
    want[k] = v
raw = open(path, 'rb').read() if os.path.exists(path) else b''
crlf = b'\r\n' in raw
text = raw.decode('utf-8', errors='surrogateescape')
lines = text.split('\r\n' if crlf else '\n')
if lines and lines[-1] == '':
    lines.pop()                      # keep a single trailing newline
cur = None; changed = 0; seen = set(); sec_end = None
for i, l in enumerate(lines):
    m = re.match(r'^\s*\[(.+?)\]\s*$', l)
    if m:
        if cur == section and sec_end is None:
            sec_end = i
        cur = m.group(1)
        continue
    if cur != section:
        continue
    m = re.match(r'^(\s*)([^;#=\s][^=]*?)(\s*=\s*)(.*)$', l)
    if not m:
        continue
    k = m.group(2)
    if k in want and k not in seen:
        seen.add(k)
        new_line = f"{m.group(1)}{k}{m.group(3)}{want[k]}"
        if change_existing and new_line != l:
            lines[i] = new_line
            changed += 1
if cur == section and sec_end is None:
    sec_end = len(lines)
missing = [k for k in want if k not in seen]
added = 0
if missing and add_missing:
    new = [f"{k}={want[k]}" for k in missing]
    if sec_end is None:              # section does not exist yet
        if lines and lines[-1] != '':
            lines.append('')
        lines += [f"[{section}]"] + new
    else:
        # insert before the blank line(s) that precede the next section
        j = sec_end
        while j > 0 and lines[j-1].strip() == '':
            j -= 1
        lines[j:j] = new
    added = len(new); missing = []
if changed or added:
    out = ('\r\n' if crlf else '\n').join(lines) + ('\r\n' if crlf else '\n')
    open(path, 'wb').write(out.encode('utf-8', errors='surrogateescape'))
print(f"{changed} changed, {added} added, missing: {' '.join(missing) or '-'}")
PY
}

# zip_has ZIP PATH : 0 when the archive contains that exact entry.
# (no `unzip | grep -q`: grep exiting early SIGPIPEs unzip, which pipefail reports as failure)
zip_has() { local l; l=$(unzip -Z1 "$1" 2>/dev/null) && grep -qxF -- "$2" <<< "$l"; }

# human-readable size
hsize() { numfmt --to=iec --suffix=B "$1" 2>/dev/null || printf '%s B' "$1"; }

# ---------------------------------------------------------------------------
# Downloads. Everything lands in tools/cache/ and is re-verified on every run.
# ---------------------------------------------------------------------------
# verify_file FILE CHECKSUM : CHECKSUM is "sha256:<hex>", "sha512:<hex>" or "" (then a
# .zip only has to pass `unzip -t`; used for FusionFix's unpinned "latest" release).
verify_file() {
  local f=$1 sum=${2:-}
  [[ -s $f ]] || return 1
  case $sum in
    sha256:*) [[ $(sha256sum "$f" | cut -d' ' -f1) == "${sum#sha256:}" ]] ;;
    sha512:*) [[ $(sha512sum "$f" | cut -d' ' -f1) == "${sum#sha512:}" ]] ;;
    '') if [[ $f == *.zip ]]; then unzip -tq "$f" >/dev/null 2>&1; fi ;;
    *) die "verify_file: bad checksum spec '$sum'" ;;
  esac
}

# fetch URL DEST [CHECKSUM] : make sure DEST exists and verifies, downloading it if not.
# Returns 2 in a dry run when the file would have to be downloaded.
fetch() {
  local url=$1 dest=$2 sum=${3:-} tmp
  if verify_file "$dest" "$sum"; then ok "cached     ${dest##*/} ($(hsize "$(stat -c %s "$dest")"))"; return 0; fi
  [[ -e $dest ]] && warn "${dest##*/} failed verification; it will be downloaded again"
  if (( DRY_RUN )); then plan "curl -fL -o $dest $url"; return 2; fi
  mkdir -p -- "$(dirname "$dest")"
  tmp="$dest.part"
  info "downloading ${dest##*/}"; dim "$url"
  if ! curl -fL --retry 3 --connect-timeout 20 --progress-bar -o "$tmp" "$url"; then
    rm -f -- "$tmp"; die "Download failed: $url"
  fi
  if ! verify_file "$tmp" "$sum"; then
    rm -f -- "$tmp"; die "${dest##*/}: checksum/integrity check failed after download"
  fi
  mv -f -- "$tmp" "$dest"
  ok "downloaded ${dest##*/} ($(hsize "$(stat -c %s "$dest")"))"
}

# ---------------------------------------------------------------------------
# Minecraft mods
# ---------------------------------------------------------------------------
# Newest dist/mods/libertycraft-*.jar produced by tools/build.sh (not -sources/-dev).
latest_lc_jar() {
  local f best=""
  for f in "$LC_DIST"/mods/libertycraft-*.jar; do
    [[ -f $f ]] || continue
    [[ $f == *-sources.jar || $f == *-dev.jar ]] && continue
    [[ -z $best || $f -nt $best ]] && best=$f
  done
  [[ -n $best ]] && printf '%s\n' "$best"
}

# install_mod_jar JAR MODS_DIR GLOB : copy JAR into MODS_DIR and delete other jars matching
# GLOB there (two versions of one mod in mods/ make Fabric refuse to start).
install_mod_jar() {
  local jar=$1 mods=$2 glob=$3 name=${1##*/} old
  [[ -d $mods ]] || run mkdir -p -- "$mods"
  for old in "$mods"/$glob; do
    [[ -e $old && ${old##*/} != "$name" ]] || continue
    run rm -f -- "$old"; info "removed old ${old##*/}"
  done
  if [[ -f "$mods/$name" ]] && cmp -s -- "$jar" "$mods/$name"; then ok "$name (already in mods/)"; return 0; fi
  run cp -f -- "$jar" "$mods/$name"
  ok "$name -> $mods/"
}
