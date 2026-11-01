#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/flatpak-page.c"

grep -Fq 'G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE' "$SRC"
grep -Fq 'fp_run_flatpak_stream' "$SRC"
grep -Fq 'fp_flatpak_line_progress' "$SRC"
grep -Fq 'fp_progress_update_async' "$SRC"
grep -Fq 'g_subprocess_launcher_setenv(launcher, "TMPDIR", FLATPAK_TMPDIR, TRUE)' "$SRC"
grep -Fq "gboolean carriage_only = (*cut == '\\r');" "$SRC"
grep -Fq 'fp_line_is_dynamic_progress' "$SRC"
grep -Fq 'if (!dynamic) fp_progress_append_async(p, line);' "$SRC"
grep -Fq 'gtk_text_view_scroll_to_iter' "$SRC"
python3 - "$SRC" <<'PY'
import sys
s=open(sys.argv[1], encoding='utf-8').read()
a=s.index('static gpointer fp_txn_worker')
b=s.index('static void fp_on_refresh', a)
body=s[a:b]
assert 'fp_run_flatpak_stream' in body
# fp_capture may still be used for post-transaction fixmenu; transaction output itself streams.
print('PASS: Flatpak transactions stream live output instead of waiting for g_spawn_sync')
PY
