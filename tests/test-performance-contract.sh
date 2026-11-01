#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
D="$ROOT/src/edpkg-gui.c"
F="$ROOT/src/flatpak-page.c"
grep -Fq 'packages-v5-' "$D"
grep -Fq 'edpkg-gui-local-loader' "$D"
grep -Fq 'edpkg-gui-catalog-loader' "$D"
grep -Fq 'GPtrArray *sorted_packages' "$D"
grep -Fq 'rebuild_sorted_packages' "$D"
grep -Fq 'gtk_tree_view_set_model(GTK_TREE_VIEW(a->tree), NULL)' "$D"
! grep -Fq 'GtkTreeModelFilter *filter' "$D"
! grep -Fq 'C_SEARCH_KEY' "$D"
grep -Fq 'GPtrArray *sorted_apps' "$F"
grep -Fq 'gtk_tree_view_set_model(GTK_TREE_VIEW(p->tree), NULL)' "$F"
! grep -Fq 'GtkTreeModelFilter *filter' "$F"
! grep -Fq 'FP_C_SEARCH_KEY' "$F"
echo 'PASS: sorted one-shot indexes, single filtering and detached GTK rebuilds'
