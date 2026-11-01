#!/bin/sh
set -eu
SRC=src/edpkg-gui.c
FP=src/flatpak-page.c
! grep -q 'C_DESC' "$SRC"
! grep -q 'C_IS_INSTALLED' "$SRC"
! grep -q 'C_ACTION_ID' "$SRC"
! grep -q 'FP_C_DESC' "$FP"
! grep -q 'FP_C_IS_INSTALLED' "$FP"
! grep -q 'FP_C_ACTION_ID' "$FP"
grep -q 'g_utf8_strlen(q,-1)<2' "$SRC"
grep -q 'name_matches<50' "$SRC"
grep -q 'name_matches<50' "$FP"
grep -q 'treeview:selected { background:#315f7a' "$SRC"
grep -q 'const char \*candidate=(a->catalog_loaded' "$SRC"
grep -q 'refresh_queued' "$SRC"
grep -q 'recompute_upgrade_flags(a);' "$SRC"
grep -q 'static int apt_simulation_kernel_check' "$SRC"
grep -q 'static int edpkg_dependency_guard' "$SRC"
grep -q 'dependency simulation failed:' "$SRC"
echo '0.1.25 optimization contract: OK'
