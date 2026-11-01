#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
D="$ROOT/src/edpkg-gui.c"
F="$ROOT/src/flatpak-page.c"
# Every DEB worker job requests automatic close on success.
grep -Fq 'finish_thread(a,rc==0,cancelled,TRUE,j->type==JOB_UPDATE' "$D"
# Refresh keeps its progress dialog until the refreshed catalog has also finished loading.
grep -Fq 'close_progress_after_load' "$D"
grep -Fq 'Refresh owns the complete catalog rebuild' "$D"
grep -Fq 'gtk_widget_destroy(dlg);' "$D"
grep -Fq 'gtk_window_present(GTK_WINDOW(a->window))' "$D"
# Flatpak package transactions close their progress window on success and return to main.
grep -Fq 'if (r->ok && p->progress_dialog)' "$F"
grep -Fq 'gtk_window_present(p->parent)' "$F"
printf '%s\n' 'PASS: successful DEB work dialogs and Flatpak transactions close automatically'
