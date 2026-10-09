#!/usr/bin/env bash
# Download the latest published LibertyCraft release and run its Linux installer.
set -euo pipefail

REPOSITORY=${LIBERTYCRAFT_REPOSITORY:-mrborghini/libertycraft}
API_URL="https://api.github.com/repos/$REPOSITORY/releases/latest"
ASSET_NAME=libertycraft-linux.tar.gz
CHECKSUM_NAME="$ASSET_NAME.sha256"

usage() {
  cat <<'EOF'
Usage: tools/install-latest.sh [install.sh options]

Downloads the latest published LibertyCraft release and runs its Linux setup.
Arguments are passed to tools/install.sh. The installer uses --yes by default.

Set LIBERTYCRAFT_REPOSITORY=owner/repository to use another GitHub repository.
EOF
}
if [[ ${1:-} == --help || ${1:-} == -h ]]; then usage; exit 0; fi

for command_name in curl python3 tar sha256sum; do
  command -v "$command_name" >/dev/null 2>&1 || {
    printf 'Missing required command: %s\n' "$command_name" >&2
    exit 1
  }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/libertycraft-release.XXXXXX")
cleanup() { rm -rf -- "$work"; }
trap cleanup EXIT

printf 'Looking up the latest published release from %s...\n' "$REPOSITORY"
curl --fail --location --silent --show-error \
  -H 'Accept: application/vnd.github+json' -H 'X-GitHub-Api-Version: 2022-11-28' \
  "$API_URL" -o "$work/release.json"
python3 - "$work/release.json" "$ASSET_NAME" "$CHECKSUM_NAME" > "$work/assets.txt" <<'PY'
import json
import sys

payload = json.load(open(sys.argv[1], encoding="utf-8"))
if payload.get("draft") or payload.get("prerelease"):
    raise SystemExit("The latest release is not a published stable release.")
assets = {asset["name"]: asset["browser_download_url"] for asset in payload.get("assets", [])}
for name in sys.argv[2:]:
    if name not in assets:
        raise SystemExit(f"Release {payload.get('tag_name', '(unknown)')} is missing {name}.")
    print(assets[name])
PY
mapfile -t asset_urls < "$work/assets.txt"
if [[ ${#asset_urls[@]} -ne 2 ]]; then
  printf 'Could not resolve release assets.\n' >&2
  exit 1
fi

archive="$work/$ASSET_NAME"
checksum="$work/$CHECKSUM_NAME"
curl --fail --location --silent --show-error "${asset_urls[0]}" -o "$archive"
curl --fail --location --silent --show-error "${asset_urls[1]}" -o "$checksum"
checksum_value=$(awk 'NR == 1 { print $1 }' "$checksum")
[[ $checksum_value =~ ^[0-9a-fA-F]{64}$ ]] || {
  printf 'Release checksum file is malformed.\n' >&2
  exit 1
}
printf '%s  %s\n' "$checksum_value" "$archive" | sha256sum --check --status || {
  printf 'Release archive checksum verification failed.\n' >&2
  exit 1
}

python3 - "$archive" <<'PY'
import sys
import tarfile

with tarfile.open(sys.argv[1], "r:gz") as archive:
    members = archive.getmembers()
    for member in members:
        if member.name.startswith("/") or ".." in member.name.split("/"):
            raise SystemExit(f"Unsafe path in release archive: {member.name}")
        if not (member.isfile() or member.isdir()):
            raise SystemExit(f"Unsupported entry in release archive: {member.name}")
    names = {member.name for member in members}
    required = {"tools/install.sh", "tools/lib/common.sh", "tools/lib/gamefiles.sh",
                "dist/plugins/LibertyCraft.asi"}
    missing = required - names
    if missing:
        raise SystemExit("Release archive is missing: " + ", ".join(sorted(missing)))
PY

tar -xzf "$archive" -C "$work"
cd "$work"
printf 'Starting LibertyCraft setup. Close GTA IV before continuing.\n'
bash tools/install.sh --yes "$@"
