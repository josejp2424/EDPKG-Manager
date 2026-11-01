#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$ROOT/src/edpkg-gui.c"
# Keep the proven PTY transaction model, but with the fixed streaming parser.
grep -Fq 'forkpty(&master' "$SRC"
grep -Fq 'GString *pending=g_string_new("")' "$SRC"
grep -Fq "gchar *cr=strchr(pending->str,'\\r')" "$SRC"
grep -Fq 'Keep draining the PTY after waitpid reports exit' "$SRC" || grep -Fq 'no more bytes arrived for 500 ms' "$SRC"
grep -Fq 'g_ptr_array_add(v,(gpointer)APTGET_BIN);g_ptr_array_add(v,"-y");' "$SRC"
! grep -Fq 'APT::Status-Fd=1' "$SRC"
! grep -Fq 'progress_bridge_reset' "$SRC"
grep -Fq 'flatpak_page_new(GTK_WINDOW(a->window),a->is_root)' "$SRC"
echo 'PASS: DEB PTY backend preserved with CR/pending/drain fixes'
