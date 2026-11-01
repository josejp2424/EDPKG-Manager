#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/flatpak-page.c"

# Cache files are explicit and persistent.
grep -Fq '/flatpak-cache-v2.tsv' "$SRC"
grep -Fq '/flatpak-icon-cache.tsv' "$SRC"

# Refresh updates AppStream and rebuilds both catalog and icon cache.
grep -Fq 'fp_refresh_appstream();' "$SRC"
grep -Fq 'icons = fp_rebuild_icon_index(apps, TRUE);' "$SRC"

# Startup reads caches first; full Flatpak queries are fallback for missing cache.
grep -Fq 'GHashTable *apps = fp_load_cache();' "$SRC"
grep -Fq 'GHashTable *icons = fp_load_icon_index();' "$SRC"

# Transactions load the persistent icon cache instead of scanning the icon tree.
python3 - "$SRC" <<'PY'
import sys
s=open(sys.argv[1], encoding='utf-8').read()
a=s.index('static gpointer fp_txn_worker')
b=s.index('static void fp_on_refresh', a)
body=s[a:b]
assert 'fp_load_icon_index()' in body
assert 'fp_rebuild_icon_index' not in body
print('PASS: transactions do not rebuild the Flatpak icon cache')
PY
