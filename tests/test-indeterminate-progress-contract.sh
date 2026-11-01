#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEB="$ROOT/src/edpkg-gui.c"
FP="$ROOT/src/flatpak-page.c"

grep -Fq 'gtk_progress_bar_pulse(GTK_PROGRESS_BAR(a->progress_bar))' "$DEB"
grep -Fq 'gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(a->progress_bar),FALSE)' "$DEB"
! grep -Fq 'gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress_bar)' "$DEB"

grep -Fq 'gtk_progress_bar_pulse(GTK_PROGRESS_BAR(p->progress_bar))' "$FP"
grep -Fq 'gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(p->progress_bar), FALSE)' "$FP"
! grep -Fq 'gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->progress_bar)' "$FP"
! grep -Fq 'gtk_progress_bar_set_text(GTK_PROGRESS_BAR(p->progress_bar), "0%")' "$FP"

printf '%s\n' 'indeterminate progress contract: OK'
