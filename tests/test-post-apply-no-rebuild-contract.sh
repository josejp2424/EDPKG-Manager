#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/edpkg-gui.c"
grep -Fq 'capture_local_state_snapshot' "$SRC"
grep -Fq 'overlay_local_state' "$SRC"
! grep -Fq 'local_set_installed' "$SRC"
! grep -Fq 'update_store_row_for_package' "$SRC"
! grep -Fq 'schedule_apply_state' "$SRC"
# Marks are not cleared before the backend runs; successful local-state overlay clears them.
python3 - "$SRC" <<'PY'
import sys
s=open(sys.argv[1],encoding='utf-8').read()
a=s.index('static void on_apply')
b=s.index('static void on_fix',a)
assert 'p->action=ACT_NONE' not in s[a:b]
a=s.index('static void overlay_local_state')
b=s.index('static void reload_data_async',a)
assert 'dst->action=ACT_NONE' in s[a:b]
print('PASS: Apply preserves marks on failure and reloads real local status on success')
PY
