#!/bin/sh
set -eu
SRC=src/edpkg-gui.c
FP=src/flatpak-page.c

grep -q 'static int edpkg_simulation_kernel_check' "$SRC"
grep -q 'g_ptr_array_add(v,"--simulate")' "$SRC"
grep -q 'static int edpkg_fix_simulation_kernel_check' "$SRC"
grep -q 'capture_guard_argv' "$SRC"
grep -q 'run_pty_exec(a,edpkg_bin_path(),av)' "$SRC"

python3 - "$SRC" <<'PY2'
import re, sys
s=open(sys.argv[1], encoding='utf-8').read()
m=re.search(r'static int edpkg_dependency_guard\(.*?\n\}', s, re.S)
assert m, 'edpkg_dependency_guard missing'
b=m.group(0)
assert 'edpkg_simulation_kernel_check' in b
assert 'APTGET_BIN' not in b
PY2

grep -q 'search_debounce_id=g_timeout_add(120,search_refilter_idle,a)' "$SRC"
grep -q 'search_debounce_id = g_timeout_add(120, fp_search_idle, p)' "$FP"

python3 - "$SRC" "$FP" <<'PY2'
import re, sys
for path, fn in [(sys.argv[1], 'package_search_rank'), (sys.argv[2], 'fp_search_rank')]:
    s=open(path, encoding='utf-8').read()
    m=re.search(r'static int '+re.escape(fn)+r'\(.*?\n\}', s, re.S)
    assert m, fn+' missing'
    b=m.group(0)
    assert 'search_key' in b
    assert 'g_utf8_strdown' not in b
PY2

grep -q '"ellipsize",PANGO_ELLIPSIZE_END' "$SRC"
grep -q 'add_text_column(a,tr(T_CANDIDATE),C_CANDIDATE,175)' "$SRC"

grep -q 'GThread \*worker_thread;' "$SRC"
grep -q 'GThread \*load_thread;' "$SRC"
grep -q 'g_thread_join' "$SRC"
grep -q 'progress_cancel_clicked' "$SRC"
grep -q 'kill(-pid,SIGINT)' "$SRC"
grep -q 'main_delete_event' "$SRC"

echo '0.1.26 regression contract: OK'
