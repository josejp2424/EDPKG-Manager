#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
python3 - <<'PY'
from pathlib import Path
import re, json, subprocess, tempfile, shutil
ai=Path('src/appimage-page.c').read_text()
main=Path('src/edpkg-gui.c').read_text()
fp=Path('src/flatpak-page.c').read_text()
assert 'essora_tab_label("DEB",GUI_LIBDIR "/edpkg-manager.png")' in main
assert 'essora_tab_label("Flatpak",GUI_LIBDIR "/essora-flatpak-64.png")' in main
assert 'essora_tab_label("AppImage",GUI_LIBDIR "/essora-appimage-64.png")' in main
assert 'gtk_widget_set_sensitive(p->btn_refresh,!busy);' in ai
assert 'gtk_widget_set_sensitive(p->btn_refresh,TRUE);' in ai
assert 'gtk_widget_set_sensitive(p->btn_refresh, TRUE);' in fp
assert 'gtk_widget_set_sensitive(a->btn_refresh,TRUE);' in main
assert '!a->busy&&!a->loading' in main
assert 'if (p->busy || (!p->backend && type!=AI_REFRESH_CATALOG && type!=AI_LOAD_LOCAL))' in ai
assert 'ai_fetch_official_catalog(p,&catalog)' in ai
assert 'g_file_get_contents(p->cache_path' in ai
assert 'g_file_set_contents(p->cache_path' in ai
assert 'p->filter=AI_SHOW_ALL;' in ai
assert 'if (p->backend) {' in ai
assert 'const gchar *const args[]={p->backend,"-fi","--byname",NULL};' in ai
assert 'ai_start(p,AI_LOAD_LOCAL,NULL);' in ai
assert 'if (p->backend) ai_start(p,AI_LOAD_LOCAL,NULL)' not in ai
assert main.count('APP_VERSION "0.1.40"')==1
# The catalog must download only in the explicit refresh worker; startup must
# never spawn curl/wget or require AM.
constructor=ai[ai.index('GtkWidget *appimage_page_new('):]
assert 'ai_fetch_official_catalog' not in constructor
assert 'AI_REFRESH_CATALOG' not in constructor
assert 'ai_find_backend' in constructor
# A minimal compilation of the real URL/command builder with mocked process
# and file-discovery routines verifies the missing-AM (curl or wget) path.
def block(text, begin):
    start=text.index(begin); brace=text.index('{',start); depth=0
    for i in range(brace,len(text)):
        depth+=(text[i]=='{')-(text[i]=='}')
        if depth==0:return text[start:i+1]
    raise AssertionError(begin)
fetch=block(ai,'static gboolean ai_fetch_official_catalog(')
program=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/utsname.h>
typedef char gchar;
typedef int gboolean;
#define TRUE 1
#define FALSE 0
#define AI_T_FAILED_LOAD 1
#define AI_T_NO_DOWNLOADER 2
typedef struct { int placeholder; } AiPage;
static int mode=0;static int exec_calls=0;static char foundurl[512];
static int g_strcmp0(const char *a,const char *b){return strcmp(a,b);}
static gchar *g_strdup_printf(const char *fmt,...){char buffer[1024];va_list va;
 va_start(va,fmt);vsnprintf(buffer,sizeof(buffer),fmt,va);va_end(va);return strdup(buffer);}
static gchar *g_find_program_in_path(const char *cmd){
 if(!strcmp(cmd,"curl") && mode==1) return strdup("/usr/bin/curl");
 if(!strcmp(cmd,"wget") && mode==2) return strdup("/usr/bin/wget");
 return NULL;}
static void g_free(void *p){free(p);}
static const char *ai_tr(int key){(void)key;return "no downloader";}
static void ai_queue_text(AiPage *p,const char *message,int status){(void)p;(void)message;(void)status;}
static gboolean ai_exec(AiPage *p,const gchar *const *argv,gchar **capture){
 (void)p;exec_calls++;
 assert(strstr(argv[0],mode==1?"curl":"wget"));
 int n=0;while(argv[n])n++;
 assert(n>=5 && strstr(argv[n-1],"https://raw.githubusercontent.com/ivan-hc/AM/main/programs/") == argv[n-1]);
 assert(strstr(argv[n-1],"-appimages") != NULL);
 snprintf(foundurl,sizeof(foundurl),"%s",argv[n-1]);
 *capture=strdup("◆ sample : test\n");return TRUE;
}
'''
main_c=r'''
int main(void){AiPage page={0};char *capture=NULL;
 mode=0;assert(!ai_fetch_official_catalog(&page,&capture));assert(exec_calls==0);
 mode=1;assert(ai_fetch_official_catalog(&page,&capture));assert(exec_calls==1);
 assert(capture && strstr(capture,"sample"));free(capture);capture=NULL;
 mode=2;assert(ai_fetch_official_catalog(&page,&capture));assert(exec_calls==2);
 free(capture);puts("AppImage official catalog works without AM (mock curl/wget): OK");return 0;}
'''
with tempfile.TemporaryDirectory() as td:
    c=Path(td)/'refresh.c';exe=Path(td)/'refresh'
    c.write_text(program+'\n'+fetch+'\n'+main_c)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Every locale contains both the missing-AM and no-downloader explanations.
translations=json.loads(Path('translations/appimage.json').read_text())
keys=translations['keys'];assert keys.index('no_downloader')==36
assert len(translations['translations'])==12
for locale,items in translations['translations'].items():
    assert len(items)==len(keys)
    assert 'AM' in items[keys.index('missing')]
    assert 'curl' in items[keys.index('no_downloader')]
print('Three clickable refresh actions, three icons, lazy startup and 12 locales: OK')
PY
