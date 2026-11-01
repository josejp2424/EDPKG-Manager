#!/bin/sh
set -eu
SRC="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/src/edpkg-gui.c"
grep -Fq 'checksum_dir_meta(sum, "/etc/apt/preferences.d")' "$SRC"
grep -Fq 'checksum_dir_meta(sum, "/etc/apt/sources.list.d")' "$SRC"
grep -Fq '# edpkg-gui-cache-v5' "$SRC"
# v5 cache contains repository metadata, not installed-state flags.
python3 - "$SRC" <<'PY'
import sys
s=open(sys.argv[1],encoding='utf-8').read()
a=s.index('static gboolean save_package_cache')
b=s.index('static gboolean load_package_cache',a)
body=s[a:b]
assert 'p->is_installed?1:0' not in body
assert 'p->is_broken?1:0' not in body
print('PASS: cache signature follows apt config directories and cache stores repo metadata only')
PY
