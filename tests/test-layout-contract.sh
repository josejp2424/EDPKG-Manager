#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$ROOT/src/edpkg-gui.c"
MAKE="$ROOT/Makefile"
DESK="$ROOT/data/edpkg-gui.desktop"
for icon in edpkg-manager package-installed package-broken package-upgradable; do
    test -f "$ROOT/data/$icon.svg"
    test -f "$ROOT/data/$icon.png"
    grep -Fq "data/$icon.svg" "$MAKE"
    grep -Fq "data/$icon.png" "$MAKE"
done
grep -Fq 'LIBDIR ?= $(PREFIX)/lib/edpkg-gui' "$MAKE"
grep -Fq 'install -Dm0755 data/edpkg-gui-launcher' "$MAKE"
grep -Fq 'Icon=/usr/lib/edpkg-gui/edpkg-manager.png' "$DESK"
grep -Fq '#define ICON_FILE GUI_LIBDIR "/edpkg-manager.png"' "$SRC"
grep -Fq '#define ICON_INSTALLED_FILE GUI_LIBDIR "/package-installed.png"' "$SRC"
grep -Fq '#define ICON_BROKEN_FILE GUI_LIBDIR "/package-broken.png"' "$SRC"
grep -Fq '#define ICON_UPGRADABLE_FILE GUI_LIBDIR "/package-upgradable.png"' "$SRC"
grep -Fq 'gtk_window_set_default_icon_from_file(ICON_FILE' "$SRC"
grep -Fq 'gtk_tree_selection_set_mode(sel,GTK_SELECTION_MULTIPLE)' "$SRC"
grep -Fq 'add_state_icon_column(a,tr(T_STATE),72)' "$SRC"
grep -Fq 'bg="#8f2424"' "$SRC"
grep -Fq 'bg="#d97818"' "$SRC"
# Installed state uses its icon; selected rows must have a distinct highlight.
! grep -Fq 'else if (p->is_installed) { *bg="#236648"' "$SRC"
grep -Fq 'treeview:selected { background:#315f7a' "$SRC"
! grep -Fq 'add_text_column(a,tr(T_STATE)' "$SRC"
echo "PASS: exact private icons, visible selection, multi-select and compact state styling"
