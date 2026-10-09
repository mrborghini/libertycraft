#!/usr/bin/env bash
# Package the current dist/ outputs and both platform installers for a GitHub release.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
VERSION=${1:-}
if [[ ! $VERSION =~ ^v?[0-9][A-Za-z0-9._+-]*$ ]]; then
  printf 'Usage: tools/package-release.sh VERSION\n' >&2
  exit 2
fi

DIST="$ROOT/dist"
[[ -f "$DIST/plugins/LibertyCraft.asi" ]] || { echo 'dist/plugins/LibertyCraft.asi is missing' >&2; exit 1; }
compgen -G "$DIST/mods/libertycraft-*.jar" >/dev/null || { echo 'dist/mods/libertycraft-*.jar is missing' >&2; exit 1; }
[[ -f "$ROOT/tools/install-windows.bat" && -f "$ROOT/tools/install-windows.ps1" ]] || {
  echo 'Windows installer files are missing.' >&2; exit 1;
}
for file in install.sh uninstall.sh lib/common.sh lib/gamefiles.sh; do
  [[ -f "$ROOT/tools/$file" ]] || { echo "tools/$file is missing" >&2; exit 1; }
done

VERSION=${VERSION#v}
OUTPUT="$DIST/release"
STAGE=$(mktemp -d "$DIST/.release-package.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT
LINUX="$STAGE/linux"
WINDOWS="$STAGE/windows"
mkdir -p "$LINUX/tools/lib" "$LINUX/dist/plugins" "$LINUX/dist/mods" "$WINDOWS/dist/plugins" "$WINDOWS/dist/mods"

cp "$ROOT/tools/install.sh" "$ROOT/tools/uninstall.sh" "$LINUX/tools/"
cp "$ROOT/tools/lib/common.sh" "$ROOT/tools/lib/gamefiles.sh" "$LINUX/tools/lib/"
cp "$ROOT/tools/install-windows.bat" "$ROOT/tools/install-windows.ps1" "$WINDOWS/"
cp "$DIST/plugins/LibertyCraft.asi" "$LINUX/dist/plugins/"
cp "$DIST/plugins/LibertyCraft.asi" "$WINDOWS/dist/plugins/"
for file in LibertyCraft.ini LibertyCraft.pdb; do
  [[ -f "$DIST/plugins/$file" ]] || continue
  cp "$DIST/plugins/$file" "$LINUX/dist/plugins/"
  cp "$DIST/plugins/$file" "$WINDOWS/dist/plugins/"
done
copied_jar=0
for jar in "$DIST"/mods/libertycraft-*.jar; do
  [[ -f $jar && $jar != *-sources.jar && $jar != *-dev.jar ]] || continue
  cp "$jar" "$LINUX/dist/mods/"
  cp "$jar" "$WINDOWS/dist/mods/"
  copied_jar=1
done
(( copied_jar )) || { echo 'No release Fabric jar found in dist/mods.' >&2; exit 1; }
cp "$ROOT/README.md" "$LINUX/README.md"
cp "$ROOT/README.md" "$WINDOWS/README.md"
mkdir -p "$OUTPUT"
rm -f "$OUTPUT/libertycraft-linux.tar.gz" "$OUTPUT/libertycraft-linux.tar.gz.sha256" \
  "$OUTPUT/libertycraft-windows.zip" "$OUTPUT/libertycraft-windows.zip.sha256" "$OUTPUT/SHA256SUMS"

tar -czf "$OUTPUT/libertycraft-linux.tar.gz" -C "$LINUX" tools dist README.md
python3 - "$WINDOWS" "$OUTPUT/libertycraft-windows.zip" <<'PY'
from pathlib import Path
import sys
import zipfile

root = Path(sys.argv[1])
with zipfile.ZipFile(sys.argv[2], "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in sorted(root.rglob("*")):
        if path.is_file():
            archive.write(path, path.relative_to(root).as_posix())
PY
(cd "$OUTPUT" && sha256sum libertycraft-linux.tar.gz > libertycraft-linux.tar.gz.sha256 && \
  sha256sum libertycraft-windows.zip > libertycraft-windows.zip.sha256 && \
  sha256sum libertycraft-linux.tar.gz libertycraft-windows.zip > SHA256SUMS)
printf 'Packaged LibertyCraft %s:\n  %s\n  %s\n' "$VERSION" \
  "$OUTPUT/libertycraft-linux.tar.gz" "$OUTPUT/libertycraft-windows.zip"
