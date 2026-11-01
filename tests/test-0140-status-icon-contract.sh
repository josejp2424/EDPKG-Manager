#!/bin/sh
# Check model column types and the installed/upgrade state mapping without GTK.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
python3 - <<'PY'
from pathlib import Path
import re, struct
fp=Path('src/flatpak-page.c').read_text()
ai=Path('src/appimage-page.c').read_text()
deb=Path('src/edpkg-gui.c').read_text()
for png in ('package-installed.png','package-upgradable.png'):
    data=(Path('data')/png).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', f'{png} is not a PNG'
    assert 16 <= struct.unpack('>I',data[16:20])[0] <= 128
# Existing DEB resource and state ordering are still the canonical reference.
assert 'if (p->is_upgradable) return a->icon_upgradable;' in deb
assert 'if (p->is_installed) return a->icon_installed;' in deb
# Model columns/types match at construction (order is important to GtkListStore).
assert re.search(r'FP_C_HAS_COLOR,\s*FP_C_STATE_ICON,.*?FP_N_COLS',fp,re.S)
assert re.search(r'gtk_list_store_new\(FP_N_COLS,.*?G_TYPE_BOOLEAN,\s*GDK_TYPE_PIXBUF\)',fp,re.S)
assert re.search(r'AI_COL_DESCRIPTION,\s*AI_COL_STATE_ICON,\s*AI_N_COLUMNS',ai,re.S)
assert re.search(r'gtk_list_store_new\(AI_N_COLUMNS,.*?G_TYPE_STRING,\s*GDK_TYPE_PIXBUF\)',ai,re.S)
# Rendering uses installed state from LOCAL inventory, not the selected action.
assert 'FP_C_STATE_ICON,a->upgradable ? p->icon_upgradable :' in fp
assert '(a->installed ? p->icon_installed : NULL)' in fp
assert 'AI_COL_STATE_ICON,a->installed ? p->icon_installed : NULL' in ai
assert 'p->icon_installed = fp_load_pixbuf(FP_INSTALLED_ICON, 24)' in fp
assert 'p->icon_upgradable = fp_load_pixbuf(FP_UPGRADABLE_ICON, 24)' in fp
assert 'p->icon_installed=gdk_pixbuf_new_from_file_at_scale(AI_INSTALLED_ICON,24,24,TRUE,NULL)' in ai
# Exactly one tab column for each type; keep Flatpak's application pixbuf too.
assert fp.count('fp_add_status_column(p);') == 1
assert fp.count('fp_add_icon_column(p, 42);') == 1
assert ai.count('ai_add_status_column(GTK_TREE_VIEW(p->tree));') == 1
assert '"pixbuf", FP_C_STATE_ICON' in fp
assert '"pixbuf",AI_COL_STATE_ICON' in ai
# Never reload any state pixbuf inside the row builder or a network task.
for src,fn in ((fp,'static void fp_append_row('),(ai,'static void ai_rebuild(')):
    chunk=src[src.index(fn):src.index('\nstatic ',src.index(fn)+12)]
    assert 'gdk_pixbuf_new_from_file' not in chunk and 'fp_load_pixbuf(' not in chunk
print('Status icons DEB/Flatpak/AppImage, column types, cached pixbufs: OK')
PY
