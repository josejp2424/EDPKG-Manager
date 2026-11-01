#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
D="$ROOT/src/debug-log.c"
H="$ROOT/src/debug-log.h"
C="$ROOT/src/edpkg-gui.c"
[ -f "$D" ] && [ -f "$H" ]
grep -Fq '#define EAPT_DEBUG_LOG_DIR "/run/edpkg-gui"' "$H"
grep -Fq 'EDPKG_GUI_DEBUG' "$D"
grep -Fq 'if (!debug_enabled) return;' "$D"
grep -Fq 'O_NOFOLLOW' "$D"
grep -Fq 'st.st_uid != geteuid()' "$D"
! grep -Fq '/tmp/edpkg-gui.log' "$H"
! grep -Fq '"draw", G_CALLBACK' "$D"
grep -Fq 'eapt_debug_session_start(argc,argv);' "$C"
echo 'PASS: diagnostic logging is opt-in and uses a protected /run path'
