#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEB="$ROOT/src/edpkg-gui.c"
FP="$ROOT/src/flatpak-page.c"
grep -Fq 'gtk_dialog_new_with_buttons(title,GTK_WINDOW(a->window),GTK_DIALOG_DESTROY_WITH_PARENT' "$DEB"
grep -Fq 'gtk_window_set_modal(GTK_WINDOW(a->progress_dialog),FALSE);' "$DEB"
grep -Fq 'progress_dialog_destroyed' "$DEB"
grep -Fq 'gtk_dialog_new_with_buttons("Flatpak", p->parent, GTK_DIALOG_DESTROY_WITH_PARENT' "$FP"
grep -Fq 'gtk_window_set_modal(GTK_WINDOW(p->progress_dialog), FALSE);' "$FP"
grep -Fq 'fp_progress_dialog_destroyed' "$FP"
grep -Fq 'G_CALLBACK(gtk_widget_destroy)' "$FP"
echo "PASS: progress windows do not leave WM-modal state behind"
