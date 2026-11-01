#!/bin/sh
set -eu
SRC=src/edpkg-gui.c
! grep -F 'g_source_remove(a->pulse_id);if(a->search_debounce_id)' "$SRC" >/dev/null
grep -F 'if (a->pulse_id)' "$SRC" >/dev/null
grep -F 'if (a->search_debounce_id)' "$SRC" >/dev/null
echo "cleanup warning contract: OK"
