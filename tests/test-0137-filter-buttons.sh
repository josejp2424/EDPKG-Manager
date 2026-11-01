#!/bin/sh
# 0.1.36: AppImage sidebar filters.  gtk_toggle_button_set_active() emits "clicked"
# in GTK3, so the handler re-entered itself and always ended on "Installed";
# after Refresh catalog the list looked empty.  Uses REAL GtkToggleButtons.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
python3 - <<'PY'
from pathlib import Path
ai=Path('src/appimage-page.c').read_text()
# programmatic changes must go through the blocking helper
assert 'ai_sync_filter_buttons' in ai, 'handler re-entry fix missing'
assert ai.count('ai_sync_filter_buttons(p)')>=3
assert 'g_signal_handlers_block_by_func(p->filters[i],G_CALLBACK(ai_filter_clicked),p)' in ai
assert 'gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->filters[1])' not in ai
assert 'gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->filters[0]),FALSE)' not in ai
def block(begin):
    st=ai.index(begin); b=ai.index('{',st); d=0
    for i in range(b,len(ai)):
        d+=(ai[i]=='{')-(ai[i]=='}')
        if d==0: return ai[st:i+1]
prog='''#include <gtk/gtk.h>
#include <assert.h>
#include <stdio.h>
typedef enum { AI_SHOW_INSTALLED, AI_SHOW_ALL, AI_SHOW_AVAILABLE } AiFilter;
typedef struct { GtkWidget *filters[3]; AiFilter filter; int rebuilds; int filter_chosen; } AiPage;
static void ai_rebuild(AiPage *p){p->rebuilds++;}
static void ai_filter_clicked(GtkButton *b,gpointer data);
'''+block('static void ai_sync_filter_buttons(')+'\n'+block('static void ai_filter_clicked(GtkButton *b,gpointer data) {')+'''
int main(void){
  if(!gtk_init_check(NULL,NULL)){puts("filter buttons: SKIPPED (no display)");return 0;}
  AiPage p={0};
  for(int i=0;i<3;i++){p.filters[i]=gtk_toggle_button_new_with_label("x");
    g_object_set_data(G_OBJECT(p.filters[i]),"ai-filter",GINT_TO_POINTER(i));
    g_signal_connect(p.filters[i],"clicked",G_CALLBACK(ai_filter_clicked),&p);}
  p.filter=AI_SHOW_INSTALLED; ai_sync_filter_buttons(&p);
  gtk_button_clicked(GTK_BUTTON(p.filters[1]));
  assert(p.filter==AI_SHOW_ALL && p.rebuilds==1);
  assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p.filters[1])));
  assert(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p.filters[0])));
  gtk_button_clicked(GTK_BUTTON(p.filters[2]));
  assert(p.filter==AI_SHOW_AVAILABLE && p.rebuilds==2);
  gtk_button_clicked(GTK_BUTTON(p.filters[0]));
  assert(p.filter==AI_SHOW_INSTALLED && p.rebuilds==3);
  p.filter=AI_SHOW_ALL; ai_sync_filter_buttons(&p);    /* what Refresh does */
  assert(p.rebuilds==3 && p.filter==AI_SHOW_ALL);
  assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p.filters[1])));
  puts("AppImage filters switch once and survive Refresh: OK");
  return 0;}
'''
import subprocess,tempfile,os,shutil
t=tempfile.mkdtemp()
Path(t,'f.c').write_text(prog)
cf=subprocess.check_output(['pkg-config','--cflags','--libs','gtk+-3.0']).decode().split()
subprocess.run(['cc','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',t+'/f.c','-o',t+'/f']+cf,check=True)
runner=[t+'/f']
if not os.environ.get('DISPLAY') and shutil.which('xvfb-run'): runner=['xvfb-run','-a']+runner
r=subprocess.run(runner,capture_output=True,text=True)
print(r.stdout.strip()); 
if r.returncode: print(r.stderr); raise SystemExit(1)
shutil.rmtree(t)
PY
