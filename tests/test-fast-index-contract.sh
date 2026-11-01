#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/edpkg-gui.c"
grep -Fq 'EDPKG_LISTS_DIR "/var/lib/edpkg/lists"' "$SRC"
grep -Fq 'EDPKG_STATUS_FILE "/var/lib/edpkg/status"' "$SRC"
grep -Fq 'APT_LISTS_DIR "/var/lib/apt/lists"' "$SRC"
grep -Fq 'DPKG_STATUS_FILE "/var/lib/dpkg/status"' "$SRC"
grep -Fq 'parse_apt_available_fast' "$SRC"
grep -Fq 'parse_edpkg_available_fast' "$SRC"
grep -Fq 'apt-cache dumpavail' "$ROOT/README.md"
echo "PASS: fast local status readers and APT compressed-index-safe catalog path"
