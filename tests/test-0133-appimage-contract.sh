#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
python3 - <<'PY'
import json, pathlib, re, struct, subprocess, tempfile
from pathlib import Path
main=Path('src/edpkg-gui.c').read_text()
ai=Path('src/appimage-page.c').read_text()
data=Path('src/appimage-data.c').read_text()
make=Path('Makefile').read_text()
fp=Path('src/flatpak-page.c').read_text()
assert '#define APP_VERSION "0.1.40"' in main
assert 'appimage-page.c' in make and 'appimage-data.c' in make and 'appimage-i18n.c' in make
assert 'essora_tab_label("AppImage"' in main
assert 'essora_tab_label("Flatpak"' in main
assert 'g_signal_connect(notebook,"switch-page",G_CALLBACK(essora_switch_tab)' in main
assert 'appimage_page_new(GTK_WINDOW(a->window),a->is_root)' in main
assert 'FLATPAK_FALLBACK_ICON "/usr/lib/edpkg-gui/essora-flatpak-64.png"' in fp
assert 'ai_start(p,AI_LOAD_LOCAL,NULL)' in ai and 'g_thread_new("edpkg-appimage"' in ai
assert 'ai_start(p,AI_LOAD_LOCAL,NULL);' in ai and 'if (p->backend) ai_start' not in ai
assert 'ai_start(data,AI_REFRESH_CATALOG,NULL)' in ai
assert 'if (j->type==AI_REFRESH_CATALOG)' in ai
assert 'g_file_get_contents(p->cache_path' in ai
assert 'g_file_set_contents(p->cache_path' in ai
assert 'AI_BUNDLED_BACKEND' in ai and 'EDPKG_APPIMAGE_EMBEDDED' in ai
assert '"-l","--appimages"' in ai
assert '"-fi","--byname"' in ai
assert '"-y",cmd,m->name,NULL' in ai
assert 'm->action==AI_INSTALL?"-ia":m->action==AI_REMOVE?"-R":"-u"' in ai
assert 'G_SUBPROCESS_FLAGS_STDIN_PIPE' in ai and 'g_output_stream_close(g_subprocess_get_stdin_pipe' in ai
assert 'g_subprocess_launcher_spawnv' in ai
assert 'g_subprocess_launcher_setenv(launch,"PAGER","cat"' in ai
assert 'gtk_scrolled_window_new(NULL,NULL)' in ai and 'gtk_dialog_run(GTK_DIALOG(dlg))' in ai
assert 'gtk_widget_set_no_show_all(p->notice,TRUE)' in ai
assert 'ai_valid_name(m->name)' not in ai or 'ai_valid_name' in data
assert 'repo_refreshed' not in ai
# Assert no network command runs in page construction except local thread; never query catalog implicitly.
constructor=ai[ai.index('GtkWidget *appimage_page_new('):]
assert 'AI_REFRESH_CATALOG' not in constructor
assert 'gtk_widget_set_sensitive(p->btn_mark,FALSE)' in constructor
# Assets must have real RGBA PNG alpha channel and correct dimensions.
for app in ('flatpak','appimage'):
  for size in (64,128,256):
    path=Path(f'data/essora-{app}-{size}.png')
    raw=path.read_bytes()
    assert raw[:8]==b'\x89PNG\r\n\x1a\n'
    w,h,depth,colour=struct.unpack('>IIBB',raw[16:26])
    assert (w,h,depth,colour)==(size,size,8,6),(path,w,h,depth,colour)
    assert f'{app}-{size}.png' in make
# Translations are separate from the APT/Flatpak catalogs, and every locale has all keys.
loc=json.loads(Path('translations/appimage.json').read_text())
assert set(loc['translations'])==set('en es pt fr de it ca hu ru ar ja zh'.split())
for code,items in loc['translations'].items():
    assert len(items)==len(loc['keys'])==41,(code,len(items))
    assert all(isinstance(x,str) and x for x in items)
    for idx,item in enumerate(items):
        if '%u' in loc['translations']['en'][idx]:
            assert item.count('%u')==1,(code,loc['keys'][idx],item)
print('AppImage integration, artwork and 12-locale contract: OK')

# Compile the ACTUAL mark-all handler with fake state, so we catch any regression that
# disables marking installed apps until repositories were refreshed.
def block(source, begin):
    start=source.index(begin)
    brace=source.index('{',start)
    depth=0
    for i in range(brace,len(source)):
        depth += (source[i]=='{')-(source[i]=='}')
        if depth==0:return source[start:i+1]
    raise AssertionError(begin)
callback=block(ai,'static void ai_on_mark_updates(')
header='''
#include <assert.h>
#include <stdio.h>
typedef void *gpointer;
typedef int GtkButton;
typedef struct { int installed; int action; } AiEntry;
typedef struct { AiEntry *items[8]; int len; } GHashTable;
typedef struct { GHashTable *table; int index; } GHashTableIter;
typedef struct { GHashTable *entries; int busy; char *backend; int count; } AiPage;
#define AI_UPDATE 3
static void g_hash_table_iter_init(GHashTableIter *it,GHashTable *h){it->table=h;it->index=0;}
static int g_hash_table_iter_next(GHashTableIter *it,void **key,void **value){
  (void)key;if(it->index==it->table->len)return 0;
  *value=it->table->items[it->index++];return 1;}
static void ai_rebuild(AiPage *p){p->count++;}
'''
program='''
int main(void){
 AiEntry installed={1,0}, other={0,0};
 GHashTable hash={.items={&installed,&other},.len=2};
 AiPage page={.entries=&hash,.busy=0,.backend="/usr/local/bin/am"};
 ai_on_mark_updates(0,&page);
 assert(installed.action==AI_UPDATE && other.action==0 && page.count==1);
 page.busy=1;installed.action=0;
 ai_on_mark_updates(0,&page);
 assert(installed.action==0);
 puts("AppImage marking without refresh gate: OK");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'t.c';exe=Path(tmp)/'t';c.write_text(header+callback+program)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'safe.c';exe=Path(tmp)/'safe'
 validator=block(data,'gboolean ai_valid_name(')
 c.write_text(r'''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
typedef int gboolean;
typedef unsigned char guchar;
typedef char gchar;
#define TRUE 1
#define FALSE 0
#define g_ascii_isalnum isalnum
''' + validator + r'''
int main(void) {
 assert(ai_valid_name("krita") && ai_valid_name("code_oss+4") && ai_valid_name("app-name.v2"));
 assert(!ai_valid_name("-R") && !ai_valid_name("../bad") && !ai_valid_name("bad/name"));
 assert(!ai_valid_name("name;id") && !ai_valid_name("a b") && !ai_valid_name(""));
 puts("AppImage argv name validation: OK");
 return 0;
}
''')
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
PY
