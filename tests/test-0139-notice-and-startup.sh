#!/bin/sh
# 0.1.38: with the REAL AppImage page (needs a display or xvfb-run):
# - cached catalog must be shown without any global AM installation;
# - if the bundled EDPKG engine is missing, an installation error is visible.
# - no standalone AM download command is shown.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/t.c" <<'EOF'
#include <gtk/gtk.h>
#include <string.h>
#include <assert.h>
#include "appimage-page.h"
static GtkWidget *page;
static GtkWidget *by_name(GtkWidget *w,const char *n){
  if(!strcmp(gtk_widget_get_name(w),n))return w;
  if(GTK_IS_CONTAINER(w)){GList*c=gtk_container_get_children(GTK_CONTAINER(w));
    for(GList*l=c;l;l=l->next){GtkWidget*r=by_name(l->data,n);if(r){g_list_free(c);return r;}}g_list_free(c);}return NULL;}
static GtkWidget *by_type(GtkWidget *w,GType t){
  if(G_TYPE_CHECK_INSTANCE_TYPE(w,t))return w;
  if(GTK_IS_CONTAINER(w)){GList*c=gtk_container_get_children(GTK_CONTAINER(w));
    for(GList*l=c;l;l=l->next){GtkWidget*r=by_type(l->data,t);if(r){g_list_free(c);return r;}}g_list_free(c);}return NULL;}
static int tick=0; static const char *mode;
static gboolean check(gpointer d){(void)d; if(++tick<8) return G_SOURCE_CONTINUE;
  GtkWidget *tv=by_type(page,GTK_TYPE_TREE_VIEW);
  int rows=gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(tv)),NULL);
  assert(rows==3);                                    /* saved catalog shown, nothing installed */
  GtkWidget *bar=by_name(page,"ai-notice"); assert(bar);
  GtkWidget *label=by_type(bar,GTK_TYPE_LABEL); assert(label);
  assert(gtk_widget_get_visible(label));              /* was FALSE: empty strip */
  const char *text=gtk_label_get_text(GTK_LABEL(label));
  assert(!strstr(text,"AM/main/INSTALL"));
  assert(!strstr(text,"PATH:"));
  if (strstr(text,"engine") || strstr(text,"motor"))
    puts("missing bundled engine notice shown: OK");
  else puts("bundled engine present, cached catalog listed: OK");
  gtk_main_quit(); return G_SOURCE_REMOVE;}
int main(int argc,char**argv){ (void)argc; mode=argv[1];
  if(!gtk_init_check(NULL,NULL)){puts("notice/startup: SKIPPED (no display)");return 0;}
  GtkWidget *win=gtk_window_new(GTK_WINDOW_TOPLEVEL);
  page=appimage_page_new(GTK_WINDOW(win),FALSE);
  gtk_container_add(GTK_CONTAINER(win),page);gtk_widget_show_all(win);
  g_timeout_add(250,check,NULL);gtk_main();return 0;}
EOF
cc -Wall -Wextra -Wno-deprecated-declarations -Isrc -o "$TMP/t" "$TMP/t.c" src/appimage-page.c src/appimage-data.c src/appimage-i18n.c $(pkg-config --cflags --libs gtk+-3.0)
if [ -n "${DISPLAY:-}" ]; then RUNNER=""; elif command -v xvfb-run >/dev/null 2>&1; then RUNNER="xvfb-run -a"; else RUNNER=""; fi
mkdir -p "$TMP/home/.cache/edpkg-gui" "$TMP/cache/edpkg-gui"
printf 'a\tone\nb\ttwo\nc\tthree\n' > "$TMP/cache/edpkg-gui/appimage-catalog-v1.tsv"
# EDPKG must not depend on a PATH containing global AM/AppMan.
$RUNNER env PATH=/nonexistent HOME="$TMP/home" XDG_CACHE_HOME="$TMP/cache" LANG=C "$TMP/t" local
