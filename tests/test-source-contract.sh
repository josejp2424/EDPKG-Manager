#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/edpkg-gui.c"
for s in 'BACKEND_EDPKG' 'BACKEND_APT' 'detect_backend' 'APTGET_BIN' 'APTCACHE_BIN' 'DPKG_BIN' 'run_backend_names' 'forkpty' 'parse_available_fast' 'parse_status_fast'; do
    grep -Fq "$s" "$SRC"
done
grep -Fq 'apt-get' "$ROOT/README.md"
grep -Fq 'dpkg --configure -a' "$ROOT/README.md"
echo "PASS: automatic EDPKG / APT-dpkg backend contract"
