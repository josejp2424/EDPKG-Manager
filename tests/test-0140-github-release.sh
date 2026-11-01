#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
python3 - <<'PYTEST'
from pathlib import Path
import re, struct
src=Path('src/appimage-page.c').read_text()
assert 'GdkPixbuf *icon_appimage' in src
assert 'p->icon_appimage=gdk_pixbuf_new_from_file_at_scale(AI_ICON,28,28,TRUE,NULL)' in src
assert re.search(r'ai_add_application_column\(GTK_TREE_VIEW\(p->tree\),ai_tr\(AI_T_NAME\),\s*AI_COL_NAME,258,p->icon_appimage\)',src)
block=src.split('static void ai_add_application_column(',1)[1].split('/* Display the installed-package indicator',1)[0]
assert 'gtk_tree_view_column_pack_start(col,image,FALSE)' in block
assert 'gtk_tree_view_column_add_attribute(col,text,"text",key)' in block
assert 'g_object_set(image,"pixbuf",icon,NULL)' in block
# No per-entry images are decoded or created when rendering catalogue rows.
rebuild=src.split('static void ai_rebuild(',1)[1].split('static void ai_update_details(',1)[0]
assert 'gdk_pixbuf_new_from_file' not in rebuild
assert 'icon_appimage' not in rebuild
for name in ('deb.png','flatpak.png','appimage.png'):
    data=Path('screenshot',name).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    assert struct.unpack('>I',data[16:20])[0] >= 800
readme=Path('README.md').read_text()
assert readme.startswith('![EDPKG Manager](data/edpkg-manager.svg)')
for name in ('deb.png','flatpak.png','appimage.png'):
    assert f'screenshot/{name}' in readme
assert len(list(Path('.').glob('CHANGELOG*'))) == 1
assert Path('third_party/am-10.6/LICENSE').exists()
print('GitHub release assets, shared AppImage row icon and licensing: OK')
PYTEST
