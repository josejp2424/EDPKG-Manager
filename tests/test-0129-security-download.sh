#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BIN="$ROOT/edpkg-gui"
SRC="$ROOT/src/edpkg-gui.c"
[ -x "$BIN" ] || { echo "edpkg-gui binary missing; run make first" >&2; exit 1; }
"$BIN" --selftest-edpkg-path-policy | grep -q 'EDPKG path policy selftest: OK'
"$BIN" --selftest-safe-download | grep -q 'safe-download selftest: OK'
grep -q 'edpkg_test_override_allowed' "$SRC"
grep -q '<token>:<index> <URI>' "$SRC"
echo '0.1.29 security/download selftests: OK'
