#!/bin/sh
set -eu
PROJECT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
python3 - "$PROJECT" <<'PYTEST'
from pathlib import Path
import tempfile,subprocess,sys
base=Path(sys.argv[1]); deb=(base/'src/edpkg-gui.c').read_text(); fp=(base/'src/flatpak-page.c').read_text()
def body(s,start):
    i=s.index(start)
    a=s.index('{',i); level=0
    for j in range(a,len(s)):
        if s[j]=='{':level+=1
        if s[j]=='}':
            level-=1
            if level==0:return s[i:j+1]
    raise AssertionError('unterminated function')
# Critical: prove the 0.1.31 accidental refresh-session lock is entirely gone.
dmark=body(deb,'static void on_mark_upgrades(')
fmark=body(fp,'static void fp_on_mark_upgrades(')
assert 'repo_refreshed' not in dmark, 'DEB update marking incorrectly requires refresh'
assert 'repo_refreshed' not in fmark, 'Flatpak update marking incorrectly requires refresh'
for label,source,func in [('DEB',deb,'static void on_apply('),('Flatpak',fp,'static void fp_on_apply('),('Flatpak action',fp,'static void fp_set_selected_action(')]:
    assert 'repo_refreshed' not in body(source,func),f'{label}: hard session refresh gate in {func}'
for label,source in [('DEB',deb),('Flatpak',fp)]:
    for line in source.splitlines():
        if 'gtk_widget_set_sensitive' in line and any(x in line for x in ('btn_mark_upgrades','btn_install','btn_reinstall')):
            assert 'repo_refreshed' not in line,f'{label} still disables package actions by session refresh: {line}'
# Both banners should be advisory and retain the original root restrictions.
assert 'if(a->repo_refreshed)gtk_widget_hide(a->repo_notice)' in deb
assert 'if(p->repo_refreshed)gtk_widget_hide(p->repo_notice)' in fp
assert 'gtk_scrolled_window_new' in body(deb,'static gboolean confirm_deb_changes(')
assert 'if(!check_root_or_warn(a))return' in body(deb,'static void on_apply(')
assert 'if (!p->is_root) { fp_warn_root(p); return; }' in body(fp,'static void fp_on_apply(')
# Compile ACTUAL mark-upgrade callback sources using a tiny fake GTK/GLib state.
header=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef void* gpointer;
typedef int GtkButton;
typedef struct {void *values[8];int count;} Table;
typedef struct {Table *table;int next;} GHashTableIter;
static void g_hash_table_iter_init(GHashTableIter *i, Table *t){i->table=t;i->next=0;}
static int g_hash_table_iter_next(GHashTableIter *i,gpointer *key,gpointer *value){
 if(i->next>=i->table->count)return 0;
 *key=0;*value=i->table->values[i->next++];return 1;
}
enum {ACT_NONE, ACT_UPGRADE};
typedef struct{const char *name;int is_upgradable;int action;} Package;
typedef struct{Table *packages;int repo_refreshed;int model;int summary;int details;} App;
static int package_is_kernel_blocked(const char *name){return strcmp(name,"linux-kernel")==0;}
static void rebuild_model(App *a){a->model++;}
static void update_summary(App *a){a->summary++;}
static void refresh_details(App *a){a->details++;}
enum {FP_ACT_NONE, FP_ACT_UPGRADE};
typedef struct{int upgradable;int action;} FpApp;
typedef struct{Table *apps;int is_root;int repo_refreshed;int model;int summary;int details;int warnings;} FpPage;
static void fp_warn_root(FpPage *p){p->warnings++;}
static void fp_rebuild_model(FpPage *p){p->model++;}
static void fp_update_status(FpPage *p){p->summary++;}
static void fp_refresh_details(FpPage *p){p->details++;}
'''
main=r'''
int main(void){
 Package a={"normal",1,ACT_NONE}, b={"old-version",0,ACT_NONE}, c={"linux-kernel",1,ACT_NONE};
 Table t={.values={&a,&b,&c},.count=3};
 App app={.packages=&t,.repo_refreshed=0};
 on_mark_upgrades(0,&app);
 assert(a.action==ACT_UPGRADE && b.action==ACT_NONE && c.action==ACT_NONE);
 assert(app.model==1 && app.summary==1 && app.details==1);
 FpApp fa={1,FP_ACT_NONE},fb={0,FP_ACT_NONE};
 Table ft={.values={&fa,&fb},.count=2};
 FpPage page={.apps=&ft,.is_root=1,.repo_refreshed=0};
 fp_on_mark_upgrades(0,&page);
 assert(fa.action==FP_ACT_UPGRADE && fb.action==FP_ACT_NONE);
 assert(page.model==1 && page.summary==1 && page.details==1);
 fa.action=FP_ACT_NONE;page.is_root=0;
 fp_on_mark_upgrades(0,&page);
 assert(fa.action==FP_ACT_NONE && page.warnings==1);
 puts("EDPKG/Flatpak marking with repo_refreshed=FALSE: OK");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as td:
    c=Path(td)/'mark.c';binary=Path(td)/'mark'
    c.write_text(header+'\n'+dmark+'\n'+fmark+'\n'+main)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True,capture_output=True,text=True)
    print(subprocess.run([str(binary)],check=True,capture_output=True,text=True).stdout,end='')
print('0.1.32 package action advisory/selection regression checks: OK')
PYTEST
