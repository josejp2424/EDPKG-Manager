#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/src/flatpak-page.c"
MAIN="$ROOT/src/edpkg-gui.c"
MAKE="$ROOT/Makefile"

test -f "$ROOT/data/app_flatpak.png"
grep -Fq 'flatpak_page_new' "$MAIN"
grep -Fq 'essora_tab_label("Flatpak",GUI_LIBDIR "/essora-flatpak-64.png")' "$MAIN"
grep -Fq '#define EAPT_ICON_INDEX EAPT_DIR "/flatpak-icon-cache.tsv"' "$SRC"
grep -Fq '#define EAPT_CATALOG EAPT_DIR "/flatpak-cache-v2.tsv"' "$SRC"
grep -Fq 'FLATPAK_EXPORT_ICONS "/var/lib/flatpak/exports/share/icons"' "$SRC"
grep -Fq 'FLATPAK_EXPORT_DESKTOP "/var/lib/flatpak/exports/share/applications"' "$SRC"
grep -Fq 'remote-add", "--if-not-exists"' "$SRC"
grep -Fq '"--system", "update", "--appstream", FLATPAK_REMOTE' "$SRC"
grep -Fq 'g_socket_client_connect_to_host(client, "flathub.org", 443' "$SRC"
grep -Fq 'FLATPAK_TMPDIR "/var/tmp/flatpak-temp"' "$SRC"
grep -Fq 'FIXMENUS_BIN "/usr/bin/fixmenus"' "$SRC"
grep -Fq 'FIXMENU_COMPAT_BIN "/usr/local/bin/fixmenu"' "$SRC"
grep -Fq 'if (menu_changed) fp_run_fixmenus();' "$SRC"
grep -Fq 'install -Dm0644 data/app_flatpak.png "$(DESTDIR)$(PREFIX)/lib/eapt/app_flatpak.png"' "$MAKE"
grep -Fq 'ln -sfn /usr/lib/eapt/app_flatpak.png "$(DESTDIR)$(DATADIR)/pixmaps/app_flatpak.png"' "$MAKE"
for lang in en es fr it pt ca de hu ru ja zh ar; do
    grep -Fq "{\"$lang\", {" "$SRC"
done
grep -Fq 'fp_scan_icon_tree(index, FLATPAK_EXPORT_ICONS, 0);' "$SRC"
grep -Fq 'fp_add_desktop_icon_aliases(index);' "$SRC"
grep -Fq 'static GHashTable *fp_build_remote_catalog' "$SRC"
grep -Fq 'g_hash_table_lookup(p->icon_index, app_id)' "$SRC"
grep -Fq 'First paint: local installed state only' "$SRC"
grep -Fq 'Refresh is the only path that may touch Flathub' "$SRC"
grep -Fq 'Transactions only refresh local installed state' "$SRC"
if grep -Fq 'EAPT_ICON_DIR' "$SRC"; then echo 'FAIL: icons must not be copied into /usr/lib/eapt/icons' >&2; exit 1; fi
echo "PASS: Flatpak persistent catalog + real icon cache + explicit full Refresh contract"
