#!/usr/bin/env bash
# tools/uninstall.sh - undo tools/install.sh: restore every game file it replaced or removed,
# delete what it added (all recorded in <gamedir>/_libertycraft_backup/manifest.txt).
# Accepts the same --dry-run / --yes flags. The Prism instance (your worlds) is kept.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/install.sh" --uninstall "$@"
