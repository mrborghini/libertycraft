#!/usr/bin/env bash
# tools/lib/gamefiles.sh: tracked, reversible changes to the GTA IV game directory.
#
# Every file we put into (or take out of) the game directory goes through install_file /
# remove_file, which keep a manifest at <gamedir>/_libertycraft_backup/manifest.txt:
#
#   added<TAB>path      the file did not exist before            -> uninstall deletes it
#   replaced<TAB>path   the original is saved in the backup dir  -> uninstall restores it
#   removed<TAB>path    the original was moved to the backup dir -> uninstall restores it
#   mkdir<TAB>path      we created this directory                -> uninstall removes it if empty
#
# Paths are relative to the game dir. A path is backed up at most once: the first time we
# touch it. Later runs see it in the manifest and never overwrite the pristine backup with
# a file we modified ourselves. The manifest is appended *before* each change, so an
# interrupted install can still be uninstalled.

[[ -n "${_LC_GAMEFILES_LOADED:-}" ]] && return 0
_LC_GAMEFILES_LOADED=1
# shellcheck source=common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

LC_BACKUP_NAME="_libertycraft_backup"
declare -gA _LC_MANIFEST=()   # rel path -> added|replaced|removed|mkdir
declare -ga _LC_MANIFEST_ORDER=()
_LC_GAME=""
LC_LAST=""                    # result of the last install_file: added|replaced|updated|unchanged|kept

backup_dir()    { printf '%s/%s\n' "$_LC_GAME" "$LC_BACKUP_NAME"; }
manifest_path() { printf '%s/manifest.txt\n' "$(backup_dir)"; }

# manifest_load GAMEDIR : read the manifest (if any) into memory. Must be called first.
manifest_load() {
  _LC_GAME=${1%/}
  _LC_MANIFEST=(); _LC_MANIFEST_ORDER=()
  local f action rel
  f=$(manifest_path)
  [[ -f $f ]] || return 0
  while IFS=$'\t' read -r action rel; do
    [[ -z $action || $action == \#* || -z $rel ]] && continue
    case $action in added|replaced|removed|mkdir) ;; *) warn "manifest: ignoring line '$action $rel'"; continue ;; esac
    [[ -z ${_LC_MANIFEST[$rel]:-} ]] && _LC_MANIFEST_ORDER+=("$rel")
    _LC_MANIFEST[$rel]=$action
  done < "$f"
}

manifest_state() { printf '%s' "${_LC_MANIFEST[$1]:-}"; }
manifest_count() { printf '%s' "${#_LC_MANIFEST_ORDER[@]}"; }

_manifest_write_header() {
  local f; f=$(manifest_path)
  [[ -f $f ]] && return 0
  mkdir -p "$(dirname "$f")"
  {
    printf '# LibertyCraft install manifest - written by tools/install.sh, read by --uninstall.\n'
    printf '# <action>\\t<path relative to the game dir>. added = delete on uninstall;\n'
    printf '# replaced/removed = restore from this backup dir; mkdir = remove if empty.\n'
  } > "$f"
}

# manifest_add ACTION REL : remember (and persist, unless dry-run) one change.
manifest_add() {
  [[ -z ${_LC_MANIFEST[$2]:-} ]] && _LC_MANIFEST_ORDER+=("$2")
  _LC_MANIFEST[$2]=$1
  (( DRY_RUN )) && return 0
  _manifest_write_header
  printf '%s\t%s\n' "$1" "$2" >> "$(manifest_path)"
}

# manifest_drop REL : forget a path (used when we delete a file we had added ourselves).
manifest_drop() {
  local rel=$1 f i
  unset '_LC_MANIFEST[$rel]'
  for i in "${!_LC_MANIFEST_ORDER[@]}"; do
    [[ ${_LC_MANIFEST_ORDER[$i]} == "$rel" ]] && unset '_LC_MANIFEST_ORDER[i]'
  done
  _LC_MANIFEST_ORDER=("${_LC_MANIFEST_ORDER[@]}")
  (( DRY_RUN )) && return 0
  f=$(manifest_path)
  [[ -f $f ]] || return 0
  awk -F'\t' -v p="$rel" '$2 != p' "$f" > "$f.tmp" && mv -f "$f.tmp" "$f"
}

# ci_resolve REL : the zips come from Windows, where paths are case-insensitive. If a path
# component already exists with different casing (pc/ vs PC/), reuse the existing name so we
# replace the file the game actually reads instead of creating a twin next to it.
ci_resolve() (
  shopt -s nullglob dotglob
  local rel=$1 out="" part e found
  [[ -e "$_LC_GAME/$rel" ]] && { printf '%s' "$rel"; exit 0; }
  local -a parts
  IFS=/ read -r -a parts <<< "$rel"
  for part in "${parts[@]}"; do
    found=$part
    if [[ ! -e "$_LC_GAME/${out:+$out/}$part" && -d "$_LC_GAME/$out" ]]; then
      for e in "$_LC_GAME/${out:+$out/}"*; do
        e=${e##*/}
        if [[ ${e,,} == "${part,,}" ]]; then found=$e; break; fi
      done
    fi
    out=${out:+$out/}$found
  done
  printf '%s' "$out"
)

# ensure_dir REL_DIR : mkdir -p, recording every directory we create.
ensure_dir() {
  local rel=${1%/} cur="" part
  local -a parts
  [[ -z $rel || $rel == . ]] && return 0
  IFS=/ read -r -a parts <<< "$rel"
  for part in "${parts[@]}"; do
    cur=${cur:+$cur/}$part
    if [[ -d "$_LC_GAME/$cur" ]]; then continue; fi
    [[ -e "$_LC_GAME/$cur" ]] && die "$cur exists but is not a directory"
    # (in a dry run the directory never appears, hence the "not recorded yet" check)
    [[ -z ${_LC_MANIFEST[$cur]:-} ]] && manifest_add mkdir "$cur"
    (( DRY_RUN )) || mkdir -- "$_LC_GAME/$cur"
  done
}

# backup_file REL : copy the current file into the backup dir, never overwriting a backup.
backup_file() {
  local rel=$1 b
  b="$(backup_dir)/$rel"
  if [[ -e $b ]]; then warn "backup of $rel already exists; keeping the older one"; return 0; fi
  if (( DRY_RUN )); then return 0; fi
  mkdir -p -- "$(dirname "$b")"
  cp -p -- "$_LC_GAME/$rel" "$b"
}

# install_file SRC REL [keep] : copy SRC to <gamedir>/REL with backup + manifest.
# With "keep", an existing file that we installed earlier is left alone (config files the
# user may have edited, e.g. ZolikaPatch.ini). Sets LC_LAST.
install_file() {
  local src=$1 rel keep=${3:-} dst state
  rel=$(ci_resolve "$2")
  dst="$_LC_GAME/$rel"; state=${_LC_MANIFEST[$rel]:-}
  [[ -f $src ]] || die "install_file: source missing: $src"
  if [[ -n $keep && -n $state && $state != removed && -e $dst ]]; then LC_LAST=kept; return 0; fi
  if [[ -f $dst ]] && cmp -s -- "$src" "$dst"; then LC_LAST=unchanged; return 0; fi
  ensure_dir "$(dirname "$rel")"
  if [[ -z $state ]]; then
    if [[ -e $dst ]]; then
      backup_file "$rel"; manifest_add replaced "$rel"; LC_LAST=replaced
    else
      manifest_add added "$rel"; LC_LAST=added
    fi
  else
    LC_LAST=updated   # ours already (or original safely in the backup): just overwrite
  fi
  (( DRY_RUN )) || cp -f -- "$src" "$dst"
}

# remove_file REL : take a file out of the game dir (backed up unless we added it).
remove_file() {
  local rel dst state
  rel=$(ci_resolve "$1")
  dst="$_LC_GAME/$rel"; state=${_LC_MANIFEST[$rel]:-}
  [[ -e $dst || -L $dst ]] || return 1
  case $state in
    added)   manifest_drop "$rel" ;;
    replaced|removed) ;;   # pristine copy is already in the backup
    *)       backup_file "$rel"; manifest_add removed "$rel" ;;
  esac
  (( DRY_RUN )) || rm -f -- "$dst"
  return 0
}

# install_tree SRC_DIR [DEST_REL] [keep-ini] : install every file below SRC_DIR.
# Prints one line per replaced file and a summary; every file with VERBOSE=1.
install_tree() {
  local src=${1%/} dest=${2:-} keep_ini=${3:-} f rel k
  local -A n=([added]=0 [replaced]=0 [updated]=0 [unchanged]=0 [kept]=0)
  local -a new_files=()
  while IFS= read -r -d '' f; do
    rel=${f#"$src"/}; rel=${dest:+$dest/}$rel
    k=""; [[ -n $keep_ini && ${rel,,} == *.ini ]] && k=keep
    install_file "$f" "$rel" $k
    n[$LC_LAST]=$(( ${n[$LC_LAST]} + 1 ))
    case $LC_LAST in
      replaced) info "replace $rel  (original backed up)" ;;
      kept)     dim "keep    $rel  (already installed; your edits are preserved)" ;;
      added)   new_files+=("$rel"); (( ${VERBOSE:-0} )) && dim "add     $rel" ;;
      updated) (( ${VERBOSE:-0} )) && dim "update  $rel" ;;
    esac
  done < <(find "$src" -type f -print0 | sort -z)
  # In a dry run show where the new files go (grouped, unless there are only a few).
  if (( DRY_RUN && ! ${VERBOSE:-0} && ${#new_files[@]} )); then
    if (( ${#new_files[@]} <= 12 )); then
      for f in "${new_files[@]}"; do plan "add $f"; done
    else
      printf '%s\n' "${new_files[@]}" | awk -F/ '{ d = (NF > 2) ? $1 "/" $2 "/" : (NF > 1 ? $1 "/" : $0); c[d]++ }
        END { for (d in c) printf "%s\t%d\n", d, c[d] }' | sort |
        while IFS=$'\t' read -r d k; do plan "add $d  ($k files)"; done
    fi
  fi
  ok "$(printf '%d new, %d replaced, %d updated, %d unchanged, %d kept' \
        "${n[added]}" "${n[replaced]}" "${n[updated]}" "${n[unchanged]}" "${n[kept]}")"
}

# uninstall_game : undo everything in the manifest, newest change first.
uninstall_game() {
  local i rel state b restored=0 deleted=0 missing=0 dirs=0
  local -a left=()
  if (( $(manifest_count) == 0 )); then
    warn "No manifest at $(manifest_path) - nothing of ours to undo."
    return 0
  fi
  for (( i=${#_LC_MANIFEST_ORDER[@]}-1; i>=0; i-- )); do
    rel=${_LC_MANIFEST_ORDER[$i]}; state=${_LC_MANIFEST[$rel]}
    b="$(backup_dir)/$rel"
    case $state in
      added)
        if [[ -e "$_LC_GAME/$rel" ]]; then
          if (( DRY_RUN )); then plan "delete  $rel"; else rm -f -- "$_LC_GAME/$rel"; fi
          deleted=$((deleted + 1))
        fi ;;
      replaced|removed)
        if [[ -f $b ]]; then
          if (( DRY_RUN )); then plan "restore $rel  ($state, from backup)"
          else mkdir -p -- "$(dirname "$_LC_GAME/$rel")"; cp -pf -- "$b" "$_LC_GAME/$rel"; fi
          restored=$((restored + 1))
        else
          warn "backup of $rel is missing - cannot restore it (Steam's 'Verify integrity' can)"
          missing=$((missing + 1))
        fi ;;
      mkdir)
        if (( DRY_RUN )); then plan "rmdir   $rel  (if empty)"; dirs=$((dirs + 1))
        elif [[ -d "$_LC_GAME/$rel" ]]; then
          if rmdir -- "$_LC_GAME/$rel" 2>/dev/null; then dirs=$((dirs + 1)); else left+=("$rel"); fi
        fi ;;
    esac
  done
  if (( DRY_RUN )); then plan "rm -rf $(backup_dir)"
  elif (( missing == 0 )); then rm -rf -- "$(backup_dir)"
  else warn "Keeping $(backup_dir) because some backups were missing."; fi
  ok "$restored restored, $deleted deleted, $dirs directories removed, $missing missing"
  (( ${#left[@]} )) && warn "Not empty, left in place (files created by the game/mods): ${left[*]}"
  return 0
}

# install_lc_plugin : copy dist/plugins/LibertyCraft.{asi,ini,pdb} into <gamedir>/plugins/.
# The .ini is only copied the first time (it is the user's config afterwards); the .pdb lets
# crash logs resolve LibertyCraft.asi addresses. Returns 1 when nothing has been built yet.
install_lc_plugin() {
  local name keep
  [[ -f "$LC_DIST/plugins/LibertyCraft.asi" ]] || return 1
  for name in LibertyCraft.asi LibertyCraft.ini LibertyCraft.pdb; do
    [[ -f "$LC_DIST/plugins/$name" ]] || continue
    keep=""; [[ $name == *.ini ]] && keep=keep
    install_file "$LC_DIST/plugins/$name" "plugins/$name" $keep
    ok "plugins/$name ($LC_LAST)"
  done
}

# track_runtime_file REL : the mods create some files on first start (FusionFix's .cfg, our
# log). If REL does not exist yet, record it as "added" now so --uninstall deletes it later.
track_runtime_file() {
  local rel=$1
  [[ -n ${_LC_MANIFEST[$rel]:-} || -e "$_LC_GAME/$rel" ]] && return 0
  manifest_add added "$rel"
}
