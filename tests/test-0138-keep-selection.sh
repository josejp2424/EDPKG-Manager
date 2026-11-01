#!/bin/sh
# 0.1.36: marking/clearing rebuilds the list store, which used to drop the
# selection and disable every Mark/Clear button.  Real GtkTreeView/GtkListStore.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
python3 - <<'PY'
from pathlib import Path
import subprocess,tempfile,os,shutil
ai=Path('src/appimage-page.c').read_text()
assert 'ai_reselect(p,kept);' in ai, 'selection is not restored after marking'
def block(begin):
    st=ai.index(begin); b=ai.index('{',st); d=0
    for i in range(b,len(ai)):
        d+=(ai[i]=='{')-(ai[i]=='}')
        if d==0: return ai[st:i+1]
prog='''#include <gtk/gtk.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum { AI_COL_ACTION, AI_COL_NAME, AI_N_COLUMNS };
typedef struct { GtkWidget *tree; GtkListStore *store; } AiPage;
'''+block('static void ai_reselect(')+'''
static void fill(AiPage *p){
  gtk_tree_view_set_model(GTK_TREE_VIEW(p->tree),NULL); gtk_list_store_clear(p->store);
  const char *n[]={"a","b","c","d","e"};
  for(int i=0;i<5;i++){GtkTreeIter it;gtk_list_store_append(p->store,&it);gtk_list_store_set(p->store,&it,AI_COL_ACTION,"",AI_COL_NAME,n[i],-1);}
  gtk_tree_view_set_model(GTK_TREE_VIEW(p->tree),GTK_TREE_MODEL(p->store));}
int main(void){
  if(!gtk_init_check(NULL,NULL)){puts("keep selection: SKIPPED (no display)");return 0;}
  AiPage p; p.store=gtk_list_store_new(AI_N_COLUMNS,G_TYPE_STRING,G_TYPE_STRING);
  p.tree=gtk_tree_view_new(); fill(&p);
  GtkTreeSelection *sel=gtk_tree_view_get_selection(GTK_TREE_VIEW(p.tree));
  gtk_tree_selection_set_mode(sel,GTK_SELECTION_MULTIPLE);
  fill(&p);                                   /* what ai_rebuild does: selection is gone */
  assert(gtk_tree_selection_count_selected_rows(sel)==0);
  GHashTable *names=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
  g_hash_table_add(names,g_strdup("b")); g_hash_table_add(names,g_strdup("d")); g_hash_table_add(names,g_strdup("gone"));
  ai_reselect(&p,names);
  assert(gtk_tree_selection_count_selected_rows(sel)==2);
  GList *rows=gtk_tree_selection_get_selected_rows(sel,NULL); GtkTreeIter it; gchar *nm;
  gtk_tree_model_get_iter(GTK_TREE_MODEL(p.store),&it,rows->data); gtk_tree_model_get(GTK_TREE_MODEL(p.store),&it,AI_COL_NAME,&nm,-1);
  assert(strcmp(nm,"b")==0); g_free(nm);
  g_list_free_full(rows,(GDestroyNotify)gtk_tree_path_free);
  GHashTable *none=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
  fill(&p); ai_reselect(&p,none);
  assert(gtk_tree_selection_count_selected_rows(sel)==0);
  puts("Mark/Clear keep the selected rows selected: OK"); return 0;}
'''
t=tempfile.mkdtemp(); Path(t,'s.c').write_text(prog)
cf=subprocess.check_output(['pkg-config','--cflags','--libs','gtk+-3.0']).decode().split()
subprocess.run(['cc','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',t+'/s.c','-o',t+'/s']+cf,check=True)
runner=[t+'/s']
if not os.environ.get('DISPLAY') and shutil.which('xvfb-run'): runner=['xvfb-run','-a']+runner
r=subprocess.run(runner,capture_output=True,text=True); print(r.stdout.strip())
if r.returncode: print(r.stderr); raise SystemExit(1)
shutil.rmtree(t)
PY
