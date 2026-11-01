#define _GNU_SOURCE
#include "appimage-page.h"
#include "appimage-data.h"
#include "appimage-i18n.h"
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>
#include <sys/utsname.h>

#define AI_CACHE "/var/cache/edpkg-gui/appimage-catalog-v1.tsv"
#define AI_ICON "/usr/lib/edpkg-gui/essora-appimage-64.png"
#define AI_INSTALLED_ICON "/usr/lib/edpkg-gui/package-installed.png"
#define AI_LOG_LIMIT 300000

typedef enum { AI_SHOW_INSTALLED, AI_SHOW_ALL, AI_SHOW_AVAILABLE } AiFilter;
typedef enum { AI_LOAD_LOCAL, AI_REFRESH_CATALOG, AI_APPLY_CHANGES, AI_UPDATE_ALL } AiJobType;
enum { AI_COL_ACTION, AI_COL_NAME, AI_COL_VERSION, AI_COL_DESCRIPTION,
       AI_COL_STATE_ICON, AI_N_COLUMNS };
typedef struct _AiPage AiPage;
typedef struct { gchar *name; AiAction action; } AiMarked;
typedef struct {
    AiPage *page; AiJobType type;
    GPtrArray *marked;
    GHashTable *entries;
    gchar *error;
    gboolean ok;
    guint catalog_count;
    gboolean from_refresh;
} AiJob;
struct _AiPage {
    GtkWindow *parent;
    GtkWidget *root, *tree, *search, *status, *notice, *detail_name, *detail_desc;
    GtkWidget *btn_refresh, *btn_mark, *btn_apply;
    GtkWidget *btn_install, *btn_remove, *btn_update, *btn_clear;
    GtkWidget *filters[3];
    GtkWidget *dialog, *progress_bar, *progress_label, *progress_close;
    GtkTextBuffer *log_buffer;
    guint pulse_source;
    guint search_source;
    GtkListStore *store;
    GdkPixbuf *icon_installed;
    GdkPixbuf *icon_appimage; /* Shared default artwork for each catalog row. */
    GHashTable *entries;
    gchar *backend;
    gchar *cache_path;
    gchar *search_key;
    gchar *last_error;
    GMutex log_mutex;
    GString *pending_log;
    gchar *pending_status;
    guint flush_source;
    AiFilter filter;
    gboolean is_root, busy, ready, refreshed, filter_chosen;
};

static void ai_marked_free(gpointer item) {
    AiMarked *m = item; if (m) {g_free(m->name); g_free(m);}
}
/* A bundled AM 10.6-derived backend is installed with EDPKG, not as a
 * system-wide AM program. Missing files mean a broken EDPKG installation.
 * Do not substitute arbitrary executables from PATH, especially as root. */
#define AI_BUNDLED_BACKEND "/usr/lib/edpkg-gui/appimage-engine/am"
static gchar *ai_find_backend(gboolean root) {
    (void)root;
    if (g_file_test(AI_BUNDLED_BACKEND,G_FILE_TEST_IS_EXECUTABLE) &&
        !g_file_test(AI_BUNDLED_BACKEND,G_FILE_TEST_IS_DIR))
        return g_strdup(AI_BUNDLED_BACKEND);
    return NULL;
}
static void ai_set_status(AiPage *p, const gchar *s) {
    if (p->status) gtk_label_set_text(GTK_LABEL(p->status), s ? s : "");
}
static guint ai_marked_count(AiPage *p) {
    guint n=0; GHashTableIter it; gpointer val;
    g_hash_table_iter_init(&it, p->entries);
    while (g_hash_table_iter_next(&it, NULL, &val)) if (((AiEntry *)val)->action) n++;
    return n;
}
static void ai_update_status(AiPage *p) {
    gchar *s;
    guint marked = ai_marked_count(p);
    if (marked) s = g_strdup_printf(ai_tr(AI_T_MARKED), marked);
    else s = g_strdup_printf(ai_tr(AI_T_ITEMS), g_hash_table_size(p->entries));
    ai_set_status(p, s); g_free(s);
}
static gint ai_compare_entries(gconstpointer va, gconstpointer vb) {
    const AiEntry *a = *(AiEntry *const *)va;
    const AiEntry *b = *(AiEntry *const *)vb;
    return g_ascii_strcasecmp(a->name, b->name);
}
static const gchar *ai_action_text(AiAction a) {
    switch (a) {
        case AI_INSTALL: return ai_tr(AI_T_INSTALL);
        case AI_REMOVE: return ai_tr(AI_T_REMOVE);
        case AI_UPDATE: return ai_tr(AI_T_UPDATE);
        default: return "";
    }
}
static void ai_rebuild(AiPage *p) {
    if (!p->store) return;
    GPtrArray *sorted = g_ptr_array_new();
    GHashTableIter it; gpointer val;
    g_hash_table_iter_init(&it, p->entries);
    while (g_hash_table_iter_next(&it, NULL, &val)) {
        AiEntry *a=val;
        if (p->filter == AI_SHOW_INSTALLED && !a->installed) continue;
        if (p->filter == AI_SHOW_AVAILABLE && a->installed) continue;
        if (p->search_key && *p->search_key) {
            gchar *name = g_utf8_casefold(a->name, -1);
            gchar *desc = g_utf8_casefold(a->description, -1);
            gboolean match = (strstr(name, p->search_key) || strstr(desc, p->search_key));
            g_free(name); g_free(desc);
            if (!match) continue;
        }
        g_ptr_array_add(sorted,a);
    }
    g_ptr_array_sort(sorted,ai_compare_entries);
    gtk_tree_view_set_model(GTK_TREE_VIEW(p->tree),NULL);
    gtk_list_store_clear(p->store);
    for (guint i=0; i<sorted->len; i++) {
        AiEntry *a = g_ptr_array_index(sorted,i);
        GtkTreeIter row;
        gtk_list_store_append(p->store,&row);
        gtk_list_store_set(p->store,&row,
                           AI_COL_ACTION,ai_action_text(a->action),
                           AI_COL_NAME,a->name,
                           AI_COL_VERSION,a->installed ? a->version : "—",
                           AI_COL_DESCRIPTION,a->description,
                           AI_COL_STATE_ICON,a->installed ? p->icon_installed : NULL,-1);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(p->tree),GTK_TREE_MODEL(p->store));
    g_ptr_array_free(sorted,TRUE);
    ai_update_status(p);
}
static void ai_update_details(AiPage *p) {
    GtkTreeSelection *sel=gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree));
    GList *rows=gtk_tree_selection_get_selected_rows(sel,NULL);
    AiEntry *a=NULL;
    if (rows && !rows->next) {
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(p->store),&iter,rows->data)) {
            gchar *name=NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(p->store),&iter,AI_COL_NAME,&name,-1);
            a=g_hash_table_lookup(p->entries,name); g_free(name);
        }
    }
    gboolean has_selection = rows != NULL;
    g_list_free_full(rows,(GDestroyNotify)gtk_tree_path_free);
    gtk_label_set_text(GTK_LABEL(p->detail_name),a ? a->name : ai_tr(AI_T_SELECT));
    gtk_label_set_text(GTK_LABEL(p->detail_desc),a ? a->description : "");
    gtk_widget_set_sensitive(p->btn_install, !p->busy && p->backend && p->ready && has_selection);
    gtk_widget_set_sensitive(p->btn_remove, !p->busy && p->backend && p->ready && has_selection);
    gtk_widget_set_sensitive(p->btn_update, !p->busy && p->backend && p->ready && has_selection);
    gtk_widget_set_sensitive(p->btn_clear, !p->busy && p->backend && p->ready && has_selection);
}
static void ai_on_selection(GtkTreeSelection *selection,gpointer user_data) {
    (void)selection; ai_update_details(user_data);
}
static gboolean ai_search_apply(gpointer data) {
    AiPage *p=data; p->search_source=0;
    const gchar *text=gtk_entry_get_text(GTK_ENTRY(p->search));
    g_free(p->search_key); p->search_key=g_utf8_casefold(text,-1);
    ai_rebuild(p); return G_SOURCE_REMOVE;
}
static void ai_search_changed(GtkEditable *e,gpointer data) {
    (void)e; AiPage *p=data;
    if (p->search_source) g_source_remove(p->search_source);
    p->search_source=g_timeout_add(160,ai_search_apply,p);
}
static void ai_filter_clicked(GtkButton *b,gpointer data);
/* GTK3 emits "clicked" from gtk_toggle_button_set_active().  Without blocking
 * our own handler, every filter change re-entered it through the other buttons
 * and the filter ended up back on "Installed" (empty list after Refresh). */
static void ai_sync_filter_buttons(AiPage *p) {
    for (int i=0; i<3; i++) {
        g_signal_handlers_block_by_func(p->filters[i],G_CALLBACK(ai_filter_clicked),p);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->filters[i]),i==(int)p->filter);
        g_signal_handlers_unblock_by_func(p->filters[i],G_CALLBACK(ai_filter_clicked),p);
    }
}
static void ai_filter_clicked(GtkButton *b,gpointer data) {
    AiPage *p=data;
    p->filter = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),"ai-filter"));
    p->filter_chosen = TRUE;
    ai_sync_filter_buttons(p);
    ai_rebuild(p);
}
/* Rebuilding the list store drops the GTK selection, which disabled every
 * Mark/Clear button right after a mark and made "Clear mark" unreachable until
 * the rows were selected again.  Re-select the same applications afterwards and
 * keep the first one in view (set_model also resets the scroll position). */
static void ai_reselect(AiPage *p,GHashTable *names) {
    GtkTreeSelection *sel=gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree));
    GtkTreeModel *model=GTK_TREE_MODEL(p->store);
    GtkTreeIter row; GtkTreePath *first=NULL;
    gboolean more=gtk_tree_model_get_iter_first(model,&row);
    while (more) {
        gchar *name=NULL;
        gtk_tree_model_get(model,&row,AI_COL_NAME,&name,-1);
        if (name && g_hash_table_contains(names,name)) {
            gtk_tree_selection_select_iter(sel,&row);
            if (!first) first=gtk_tree_model_get_path(model,&row);
        }
        g_free(name);
        more=gtk_tree_model_iter_next(model,&row);
    }
    if (first) {
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(p->tree),first,NULL,TRUE,0.5f,0.0f);
        gtk_tree_path_free(first);
    }
}
static void ai_set_selected(AiPage *p,AiAction action) {
    if (p->busy || !p->backend) return;
    GtkTreeSelection *sel=gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree));
    GList *rows=gtk_tree_selection_get_selected_rows(sel,NULL);
    GHashTable *kept=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
    for (GList *l=rows; l; l=l->next) {
        GtkTreeIter row; gchar *name=NULL;
        if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(p->store),&row,l->data)) continue;
        gtk_tree_model_get(GTK_TREE_MODEL(p->store),&row,AI_COL_NAME,&name,-1);
        AiEntry *a=g_hash_table_lookup(p->entries,name);
        if (a && (action==AI_NONE ||
            (action==AI_INSTALL && !a->installed) ||
            ((action==AI_REMOVE || action==AI_UPDATE) && a->installed))) a->action=action;
        if (name) g_hash_table_add(kept,g_strdup(name));
        g_free(name);
    }
    g_list_free_full(rows,(GDestroyNotify)gtk_tree_path_free);
    ai_rebuild(p);
    ai_reselect(p,kept);
    g_hash_table_unref(kept);
}
static void ai_on_install(GtkButton *b,gpointer data){(void)b;ai_set_selected(data,AI_INSTALL);}
static void ai_on_remove(GtkButton *b,gpointer data){(void)b;ai_set_selected(data,AI_REMOVE);}
static void ai_on_update(GtkButton *b,gpointer data){(void)b;ai_set_selected(data,AI_UPDATE);}
static void ai_on_clear(GtkButton *b,gpointer data){(void)b;ai_set_selected(data,AI_NONE);}
static void ai_on_mark_updates(GtkButton *b,gpointer data) {
    (void)b; AiPage *p=data; if (p->busy || !p->backend) return;
    GHashTableIter it; gpointer value; g_hash_table_iter_init(&it,p->entries);
    while (g_hash_table_iter_next(&it,NULL,&value)) {
        AiEntry *a=value; if (a->installed) a->action=AI_UPDATE;
    }
    ai_rebuild(p);
}
static gchar *ai_format_header(AiPage *p) {
    GString *note=g_string_new("");
    if (!p->backend) {
        g_string_append(note,ai_tr(AI_T_MISSING));
    }
    if (!p->refreshed) {
        if (note->len) g_string_append_c(note,'\n');
        g_string_append(note,ai_tr(AI_T_NOTICE));
        if (!g_file_test(p->cache_path,G_FILE_TEST_EXISTS)) {
            g_string_append_c(note,'\n');
            g_string_append(note,ai_tr(AI_T_NO_CATALOG));
        }
    }
    /* Errors stay visible after closing the progress dialog. */
    if (p->last_error && *p->last_error) {
        if (note->len) g_string_append_c(note,'\n');
        g_string_append(note,p->last_error);
    }
    return g_string_free(note,FALSE);
}
static void ai_notice_update(AiPage *p) {
    gchar *note=ai_format_header(p);
    if (p->notice) {
        GtkWidget *label=g_object_get_data(G_OBJECT(p->notice),"ai-label");
        gtk_label_set_text(GTK_LABEL(label),note);
        gtk_widget_set_visible(p->notice,*note!=0);
    }
    g_free(note);
}
static void ai_queue_text(AiPage *p,const gchar *text,gboolean status) {
    g_mutex_lock(&p->log_mutex);
    if (status) {g_free(p->pending_status);p->pending_status=g_strdup(text);}
    else if (p->pending_log && text) {
        if (p->pending_log->len < AI_LOG_LIMIT) g_string_append_len(p->pending_log,text,MIN(strlen(text),AI_LOG_LIMIT-p->pending_log->len));
    }
    g_mutex_unlock(&p->log_mutex);
}
static gboolean ai_flush_log(gpointer data) {
    AiPage *p=data;
    gchar *update=NULL,*status=NULL;
    g_mutex_lock(&p->log_mutex);
    if (p->pending_log && p->pending_log->len) {
        update=g_strdup(p->pending_log->str);g_string_truncate(p->pending_log,0);
    }
    status=p->pending_status; p->pending_status=NULL;
    g_mutex_unlock(&p->log_mutex);
    if (status) {
        if (p->progress_label) gtk_label_set_text(GTK_LABEL(p->progress_label),status);
        ai_set_status(p,status);g_free(status);
    }
    if (update && p->log_buffer) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(p->log_buffer,&end);
        gtk_text_buffer_insert(p->log_buffer,&end,update,-1);
        /* Avoid unbounded memory growth even if an installer logs for hours. */
        if (gtk_text_buffer_get_char_count(p->log_buffer)>AI_LOG_LIMIT) {
            GtkTextIter start,trim;
            gtk_text_buffer_get_start_iter(p->log_buffer,&start);
            gtk_text_buffer_get_iter_at_offset(p->log_buffer,&trim,AI_LOG_LIMIT/3);
            gtk_text_buffer_delete(p->log_buffer,&start,&trim);
        }
    }
    g_free(update);return G_SOURCE_CONTINUE;
}
static gboolean ai_pulse(gpointer data){
    AiPage *p=data;if(p->progress_bar) gtk_progress_bar_pulse(GTK_PROGRESS_BAR(p->progress_bar));
    return G_SOURCE_CONTINUE;
}
static void ai_progress_response(GtkDialog *dlg,gint response,gpointer data) {
    (void)response;AiPage *p=data;
    if (p->busy) return;
    gtk_widget_destroy(GTK_WIDGET(dlg));
    p->dialog=NULL;p->log_buffer=NULL;p->progress_label=NULL;p->progress_bar=NULL;p->progress_close=NULL;
}
static void ai_progress_begin(AiPage *p,const gchar *text) {
    p->dialog=gtk_dialog_new_with_buttons(ai_tr(AI_T_TITLE),p->parent,
              GTK_DIALOG_DESTROY_WITH_PARENT,ai_tr(AI_T_CLOSE),GTK_RESPONSE_CLOSE,NULL);
    gtk_window_set_default_size(GTK_WINDOW(p->dialog),720,430);
    gtk_window_set_position(GTK_WINDOW(p->dialog),GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_window_set_deletable(GTK_WINDOW(p->dialog),FALSE);
    p->progress_close=gtk_dialog_get_widget_for_response(GTK_DIALOG(p->dialog),GTK_RESPONSE_CLOSE);
    gtk_widget_set_sensitive(p->progress_close,FALSE);
    GtkWidget *area=gtk_dialog_get_content_area(GTK_DIALOG(p->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area),12);
    p->progress_label=gtk_label_new(text);gtk_label_set_xalign(GTK_LABEL(p->progress_label),0);
    gtk_box_pack_start(GTK_BOX(area),p->progress_label,FALSE,FALSE,6);
    p->progress_bar=gtk_progress_bar_new();gtk_box_pack_start(GTK_BOX(area),p->progress_bar,FALSE,FALSE,0);
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    GtkWidget *tv=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(tv),FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv),GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll),tv);
    p->log_buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_box_pack_start(GTK_BOX(area),scroll,TRUE,TRUE,0);
    g_signal_connect(p->dialog,"response",G_CALLBACK(ai_progress_response),p);
    gtk_widget_show_all(p->dialog);
    p->pulse_source=g_timeout_add(120,ai_pulse,p);
}
static gboolean ai_confirm(AiPage *p,AiJob *j) {
    GtkWidget *dlg=gtk_dialog_new_with_buttons(ai_tr(AI_T_TITLE),p->parent,GTK_DIALOG_MODAL|
                     GTK_DIALOG_DESTROY_WITH_PARENT,ai_tr(AI_T_CANCEL),GTK_RESPONSE_CANCEL,
                     ai_tr(AI_T_APPLY),GTK_RESPONSE_OK,NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg),620,440);
    gtk_window_set_position(GTK_WINDOW(dlg),GTK_WIN_POS_CENTER_ON_PARENT);
    GtkWidget *area=gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *title=gtk_label_new(j->type==AI_UPDATE_ALL?ai_tr(AI_T_CONFIRM_UPDATES):ai_tr(AI_T_CONFIRM));
    gtk_label_set_xalign(GTK_LABEL(title),0);gtk_label_set_line_wrap(GTK_LABEL(title),TRUE);
    gtk_widget_set_margin_start(title,12);gtk_widget_set_margin_top(title,12);
    gtk_box_pack_start(GTK_BOX(area),title,FALSE,FALSE,4);
    GtkWidget *note=gtk_label_new(ai_tr(AI_T_UPDATE_INFO));
    gtk_label_set_xalign(GTK_LABEL(note),0);gtk_label_set_line_wrap(GTK_LABEL(note),TRUE);
    gtk_widget_set_margin_start(note,12);gtk_widget_set_margin_end(note,12);
    gtk_box_pack_start(GTK_BOX(area),note,FALSE,FALSE,4);
    GtkWidget *risk=gtk_label_new(ai_tr(AI_T_SCRIPT_WARNING));
    gtk_label_set_xalign(GTK_LABEL(risk),0);
    gtk_label_set_line_wrap(GTK_LABEL(risk),TRUE);
    gtk_widget_set_margin_start(risk,12);
    gtk_widget_set_margin_end(risk,12);
    gtk_box_pack_start(GTK_BOX(area),risk,FALSE,FALSE,4);
    GtkWidget *sc=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    GtkWidget *tv=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(tv),FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv),FALSE);
    gtk_container_add(GTK_CONTAINER(sc),tv);
    GtkTextBuffer *buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GString *text=g_string_new("");
    if (j->type==AI_UPDATE_ALL) g_string_append(text,ai_tr(AI_T_MARK_UPDATES));
    for (guint i=0;j->marked && i<j->marked->len;i++) {
        AiMarked *m=g_ptr_array_index(j->marked,i);
        g_string_append_printf(text,"%s    %s\n",ai_action_text(m->action),m->name);
    }
    gtk_text_buffer_set_text(buffer,text->str,-1);g_string_free(text,TRUE);
    gtk_box_pack_start(GTK_BOX(area),sc,TRUE,TRUE,8);
    gtk_widget_show_all(dlg);
    gint response=gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return response==GTK_RESPONSE_OK;
}
static gboolean ai_exec(AiPage *p,const gchar *const *argv,gchar **capture) {
    GSubprocessFlags flags=G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_MERGE|
                           G_SUBPROCESS_FLAGS_STDIN_PIPE;
    GSubprocessLauncher *launch=g_subprocess_launcher_new(flags);
    g_subprocess_launcher_setenv(launch,"NO_COLOR","1",TRUE);
    g_subprocess_launcher_setenv(launch,"PAGER","cat",TRUE);
    g_subprocess_launcher_setenv(launch,"GIT_PAGER","cat",TRUE);
    g_subprocess_launcher_setenv(launch,"TERM","dumb",TRUE);
    /* Run the vendored engine in system mode, never the installed AM command.
     * Its modules are shipped in a read-only, root-owned package directory. */
    g_subprocess_launcher_setenv(launch,"EDPKG_APPIMAGE_EMBEDDED","1",TRUE);
    g_subprocess_launcher_setenv(launch,"CI","1",TRUE);
    g_subprocess_launcher_setenv(launch,"LANGUAGE","en",TRUE);
    /* AM aborts with "Please do not use sudo" whenever SUDO_USER or DOAS_USER
     * is set while running as root.  This GUI is started through gksu (sudo),
     * so it inherits them; AM calls sudo itself when it needs privileges. */
    g_subprocess_launcher_unsetenv(launch,"SUDO_USER");
    g_subprocess_launcher_unsetenv(launch,"SUDO_UID");
    g_subprocess_launcher_unsetenv(launch,"SUDO_GID");
    g_subprocess_launcher_unsetenv(launch,"SUDO_COMMAND");
    g_subprocess_launcher_unsetenv(launch,"DOAS_USER");
    GError *err=NULL;
    GSubprocess *proc=g_subprocess_launcher_spawnv(launch,argv,&err);
    g_object_unref(launch);
    if (!proc) {
        if (err) {ai_queue_text(p,err->message,FALSE);ai_queue_text(p,"\n",FALSE);g_error_free(err);}
        return FALSE;
    }
    /* AM receives immediate EOF if it tries to ask for console input. */
    g_output_stream_close(g_subprocess_get_stdin_pipe(proc),NULL,NULL);
    GInputStream *stream=g_subprocess_get_stdout_pipe(proc);
    GByteArray *bytes=g_byte_array_new();
    guint8 buf[4096]; gssize n;
    while ((n=g_input_stream_read(stream,buf,sizeof(buf),NULL,&err))>0) {
        if (bytes->len < 6*1024*1024) g_byte_array_append(bytes,buf,MIN((gsize)n,6*1024*1024-bytes->len));
        if (p->dialog) {
            gchar *utf8=g_utf8_make_valid((const gchar*)buf,n);
            ai_queue_text(p,utf8,FALSE);g_free(utf8);
        }
    }
    gboolean waited=g_subprocess_wait(proc,NULL,&err);
    gboolean ok=(waited && n>=0 && g_subprocess_get_successful(proc));
    if (err) {ai_queue_text(p,err->message,FALSE);ai_queue_text(p,"\n",FALSE);g_clear_error(&err);}
    g_byte_array_append(bytes,(const guint8*)"",1);
    if (capture) *capture=g_utf8_make_valid((const gchar*)bytes->data,-1);
    g_byte_array_unref(bytes);g_object_unref(proc);
    return ok;
}
/* The adapted AM backend may exit with status 0 even when an install failed; it marks
 * every error/abort with the skull emoji instead (checked on AM 10.6-6).  The
 * marker is language independent, so it is used for AM actions and listings.
 * Do NOT use it on catalog text: descriptions are arbitrary third-party data. */
static gboolean ai_exec_am(AiPage *p,const gchar *const *argv,gchar **capture) {
    gchar *text=NULL;
    gboolean ok=ai_exec(p,argv,&text);
    /* Ordinary local listings may contain nonfatal warnings. Treat shell
     * failures as fatal only for modifying transactions (-y ...). */
    gboolean mutating=(argv && argv[1] && g_strcmp0(argv[1],"-y")==0);
    if (ok && text && (strstr(text,"\xF0\x9F\x92\x80") ||
                       (mutating && (strstr(text,"command not found") ||
                                     strstr(text,"Permission denied"))))) ok=FALSE;
    if (capture) *capture=text; else g_free(text);
    return ok;
}
/* PyMenu/Fluxbox may cache desktop menus. Match Flatpak: rebuild only
 * after a real AppImage transaction, never on tab open or catalog refresh. */
static void ai_run_fixmenus(AiPage *p) {
    static const gchar *const candidates[] = {
        "/usr/bin/fixmenus", "/usr/local/bin/fixmenus",
        "/usr/local/bin/fixmenu", "/usr/bin/fixmenu", NULL
    };
    for (guint i=0;candidates[i];i++) {
        if (!g_file_test(candidates[i],G_FILE_TEST_IS_EXECUTABLE)) continue;
        const gchar *const args[]={candidates[i],NULL};
        (void)ai_exec(p,args,NULL);
        return;
    }
    gchar *path=g_find_program_in_path("fixmenus");
    if (!path) path=g_find_program_in_path("fixmenu");
    if (path) {
        const gchar *const args[]={path,NULL};
        (void)ai_exec(p,args,NULL);
        g_free(path);
    }
}
/* First line of AM output that explains an error, for the status bar. */
static gchar *ai_error_line(const gchar *text) {
    if (!text) return NULL;
    gchar **lines=g_strsplit(text,"\n",-1);
    gchar *found=NULL;
    for (guint i=0;lines[i] && !found;i++) {
        if (strstr(lines[i],"\xF0\x9F\x92\x80") || strstr(lines[i],"ERROR") ||
            strstr(lines[i],"Error") || strstr(lines[i],"command not found") ||
            strstr(lines[i],"Permission denied")) {
            gchar *line=g_strstrip(lines[i]);
            if (*line) found=g_strndup(line,200);
        }
    }
    g_strfreev(lines);
    return found;
}
/* Download ONLY on explicit Refresh. The published AM list is the source
 * for `am -l --appimages`; browsing it does not require the AM executable.
 * A fixed HTTPS endpoint and an allow-listed architecture avoid untrusted
 * commands/URLs. The subprocess reader has a 6 MiB capture limit. */
static gboolean ai_fetch_official_catalog(AiPage *p, gchar **capture) {
    struct utsname system_info;
    const gchar *arch = "x86_64";
    if (uname(&system_info) == 0) {
        if (g_strcmp0(system_info.machine,"aarch64") == 0 ||
            g_strcmp0(system_info.machine,"arm64") == 0) arch="aarch64";
        else if (g_strcmp0(system_info.machine,"i686") == 0 ||
                 g_strcmp0(system_info.machine,"i386") == 0) arch="i686";
        else if (g_strcmp0(system_info.machine,"x86_64") != 0) {
            ai_queue_text(p,ai_tr(AI_T_FAILED_LOAD),FALSE);
            return FALSE;
        }
    }
    gchar *url=g_strdup_printf(
        "https://raw.githubusercontent.com/ivan-hc/AM/main/programs/%s-appimages",arch);
    gchar *curl=g_find_program_in_path("curl");
    gchar *wget=g_find_program_in_path("wget");
    gboolean success=FALSE;
    if (curl) {
        const gchar *const argv[]={curl,"--fail","--location","--silent",
            "--show-error","--connect-timeout","8","--max-time","45",
            "--max-filesize","6000000",url,NULL};
        success=ai_exec(p,argv,capture);
    }
    /* Retry with wget when curl fails (TLS, proxy, missing CA, etc.). */
    if (!success && wget) {
        if (capture) { g_free(*capture); *capture=NULL; }
        const gchar *const argv[]={wget,"--timeout=12","--tries=1",
            "-O","-",url,NULL};
        success=ai_exec(p,argv,capture);
    }
    if (!curl && !wget) {
        ai_queue_text(p,ai_tr(AI_T_NO_DOWNLOADER),FALSE);
        ai_queue_text(p,"\n",FALSE);
    }
    g_free(curl);g_free(wget);g_free(url);
    return success;
}
static void ai_job_free(AiJob *j) {
    if (j->entries) g_hash_table_unref(j->entries);
    if (j->marked) g_ptr_array_unref(j->marked);
    g_free(j->error); g_free(j);
}
static void ai_controls_busy(AiPage *p,gboolean busy) {
    p->busy=busy;
    gtk_widget_set_sensitive(p->btn_refresh,!busy);
    gtk_widget_set_sensitive(p->btn_mark,!busy && p->backend && p->ready);
    gtk_widget_set_sensitive(p->btn_apply,!busy && p->backend && p->ready);
    gtk_widget_set_sensitive(p->search,!busy);
    for (int i=0;i<3;i++) gtk_widget_set_sensitive(p->filters[i],!busy);
    ai_update_details(p);
}
static gboolean ai_job_done(gpointer data) {
    AiJob *j=data; AiPage *p=j->page;
    if (j->entries) {
        /* On failure retain the old catalog; local entries still refresh from disk. */
        if (j->ok || j->type==AI_LOAD_LOCAL || j->type==AI_APPLY_CHANGES || j->type==AI_UPDATE_ALL) {
            GHashTableIter it; gpointer value;
            g_hash_table_iter_init(&it,j->entries);
            while (g_hash_table_iter_next(&it,NULL,&value)) {
                AiEntry *n=value;
                AiEntry *old=g_hash_table_lookup(p->entries,n->name);
                if (old && j->type==AI_LOAD_LOCAL) n->action=old->action;
                if (old && !j->ok && j->type==AI_APPLY_CHANGES) n->action=old->action;
            }
            g_hash_table_unref(p->entries);
            p->entries=j->entries;j->entries=NULL;
        }
    }
    if (j->type==AI_REFRESH_CATALOG && j->ok) {
        g_clear_pointer(&p->last_error,g_free);
        p->refreshed=TRUE;
        /* Make the newly downloaded catalog visible, even without AM or
         * previously installed applications. */
        p->filter=AI_SHOW_ALL;
        if (p->search && *gtk_entry_get_text(GTK_ENTRY(p->search)))
            gtk_entry_set_text(GTK_ENTRY(p->search),"");
        ai_sync_filter_buttons(p);
    }
    if (j->type==AI_REFRESH_CATALOG && !j->ok) {
        g_free(p->last_error);
        p->last_error=g_strdup(j->error ? j->error : ai_tr(AI_T_FAILED_LOAD));
        /* An old cache may be browsed even if this refresh failed. */
        if (g_hash_table_size(p->entries)>0) {
            p->filter=AI_SHOW_ALL;
            ai_sync_filter_buttons(p);
        }
    }
    /* The saved catalog is kept between runs.  When the page opens with nothing
     * installed, "Installed" would be an empty list that looks like "no catalog",
     * so start on "All" (once, and never over a choice the user already made). */
    if (j->type==AI_LOAD_LOCAL && !p->ready && !p->filter_chosen && p->filter==AI_SHOW_INSTALLED) {
        gboolean any_installed=FALSE; GHashTableIter it2; gpointer v2;
        g_hash_table_iter_init(&it2,p->entries);
        while (g_hash_table_iter_next(&it2,NULL,&v2)) if (((AiEntry *)v2)->installed) {any_installed=TRUE;break;}
        if (!any_installed && g_hash_table_size(p->entries)>0) {
            p->filter=AI_SHOW_ALL;
            ai_sync_filter_buttons(p);
        }
    }
    p->ready=TRUE;
    if (p->pulse_source) {g_source_remove(p->pulse_source);p->pulse_source=0;}
    if (p->flush_source) {ai_flush_log(p);g_source_remove(p->flush_source);p->flush_source=0;}
    ai_controls_busy(p,FALSE);
    ai_notice_update(p);
    ai_rebuild(p);
    ai_update_details(p);
    if (p->dialog) {
        gtk_widget_set_sensitive(p->progress_close,TRUE);
        gtk_window_set_deletable(GTK_WINDOW(p->dialog),TRUE);
        gtk_label_set_text(GTK_LABEL(p->progress_label),j->ok?ai_tr(AI_T_DONE):ai_tr(AI_T_FAILED));
        /* Successful work dialogs close like DEB/Flatpak; failures retain the log. */
        if (j->ok) gtk_dialog_response(GTK_DIALOG(p->dialog),GTK_RESPONSE_CLOSE);
    }
    if (!j->ok) {
        ai_set_status(p,j->error ? j->error : ai_tr(AI_T_FAILED_LOAD));
    } else ai_update_status(p);
    ai_job_free(j);
    return G_SOURCE_REMOVE;
}
static gpointer ai_job_worker(gpointer data) {
    AiJob *j=data; AiPage *p=j->page;
    j->entries=ai_entries_new();
    gchar *catalog=NULL,*installed=NULL;
    GError *e=NULL;
    g_file_get_contents(p->cache_path,&catalog,NULL,NULL);
    guint cached=ai_parse_catalog_cache(j->entries,catalog);
    g_free(catalog);catalog=NULL;
    if (j->type==AI_REFRESH_CATALOG) {
        ai_queue_text(p,ai_tr(AI_T_REFRESHING),TRUE);
        ai_queue_text(p,"\n",FALSE);
        GHashTable *fresh=ai_entries_new();
        /* Browse the published list without running the installation engine. */
        gboolean ok=ai_fetch_official_catalog(p,&catalog);
        guint parsed=ok ? ai_parse_catalog(fresh,catalog) : 0;
        /* A successful HTTP response is NOT a valid catalog unless it contains
         * recognizable entries. Older AM versions can be an alternate source. */
        if ((!ok || parsed==0) && p->backend) {
            g_clear_pointer(&catalog,g_free);
            g_hash_table_remove_all(fresh);
            const gchar *const args[]={p->backend,"-l","--appimages",NULL};
            ok=ai_exec(p,args,&catalog);
            parsed=ok ? ai_parse_catalog(fresh,catalog) : 0;
        }
        if (ok && parsed>0) {
            gchar *cache=ai_create_cache(fresh);
            gchar *cache_dir=g_path_get_dirname(p->cache_path);
            gboolean saved=(g_mkdir_with_parents(cache_dir,0755)==0 &&
                            g_file_set_contents(p->cache_path,cache,-1,&e));
            g_free(cache_dir);
            if (saved) {
                g_hash_table_unref(j->entries);j->entries=fresh;
                j->catalog_count=parsed;
                j->ok=TRUE;
                gchar *result=g_strdup_printf(ai_tr(AI_T_CATALOG_LOADED),parsed);
                ai_queue_text(p,result,FALSE);ai_queue_text(p,"\n",FALSE);
                g_free(result);
            } else {
                j->error=g_strdup_printf("%s %s",ai_tr(AI_T_CACHE_FAILED),
                    e ? e->message : "");
                ai_queue_text(p,j->error,FALSE);ai_queue_text(p,"\n",FALSE);
                g_hash_table_unref(fresh);
            }
            g_free(cache);
        } else {
            gchar *downloader=g_find_program_in_path("curl");
            gchar *wget=g_find_program_in_path("wget");
            j->error=g_strdup(!downloader && !wget ? ai_tr(AI_T_NO_DOWNLOADER) :
                (ok ? ai_tr(AI_T_EMPTY_CATALOG) : ai_tr(AI_T_FAILED_LOAD)));
            ai_queue_text(p,j->error,FALSE);ai_queue_text(p,"\n",FALSE);
            g_free(downloader);g_free(wget);
            g_hash_table_unref(fresh);
        }
        g_free(catalog); catalog=NULL;
    } else if (j->type==AI_LOAD_LOCAL) j->ok=TRUE;
    else {
        gboolean menu_changed=FALSE;
        j->ok=TRUE;
        if (j->type==AI_UPDATE_ALL) {
            ai_queue_text(p,ai_tr(AI_T_RUNNING),TRUE);
            const gchar *args[]={p->backend,"-y","-u","--apps",NULL};
            j->ok=ai_exec_am(p,args,NULL);
            menu_changed=j->ok;
        } else {
            for (guint i=0;j->marked && i<j->marked->len;i++) {
                AiMarked *m=g_ptr_array_index(j->marked,i);
                gchar *label=g_strdup_printf("%s: %s",ai_action_text(m->action),m->name);
                ai_queue_text(p,label,TRUE);g_free(label);
                const gchar *cmd=m->action==AI_INSTALL?"-ia":m->action==AI_REMOVE?"-R":"-u";
                const gchar *args[]={p->backend,"-y",cmd,m->name,NULL};
                gboolean done=ai_exec_am(p,args,NULL);
                if (!done) {j->ok=FALSE; break;}
                menu_changed=TRUE;
            }
        }
        if (menu_changed) ai_run_fixmenus(p);
    }
    /* No backend still permits cached browsing and catalog updates. */
    if (p->backend) {
        const gchar *const args[]={p->backend,"-fi","--byname",NULL};
        gboolean local_ok=ai_exec_am(p,args,&installed);
        if (local_ok) ai_parse_installed(j->entries,installed);
        else if (j->type==AI_LOAD_LOCAL || j->type==AI_APPLY_CHANGES || j->type==AI_UPDATE_ALL) {
            j->ok=FALSE;
            if (!j->error) j->error=ai_error_line(installed);
        }
        /* AM reports success for an install/remove that did nothing, so confirm
         * the result against the fresh list of installed AppImages. */
        if (local_ok && j->ok && j->type==AI_APPLY_CHANGES) {
            for (guint i=0;j->marked && i<j->marked->len;i++) {
                AiMarked *m=g_ptr_array_index(j->marked,i);
                AiEntry *now=g_hash_table_lookup(j->entries,m->name);
                gboolean present=(now && now->installed);
                if ((m->action==AI_INSTALL && !present) || (m->action==AI_REMOVE && present)) {
                    gchar *label=g_strdup_printf("✖ %s: %s\n",ai_action_text(m->action),m->name);
                    ai_queue_text(p,label,FALSE);g_free(label);
                    j->ok=FALSE;
                }
            }
        }
        g_free(installed);
    }
    if (!j->ok && !j->error) j->error=g_strdup(ai_tr(AI_T_FAILED));
    if (e) g_error_free(e);
    g_idle_add(ai_job_done,j);
    return NULL;
}
static void ai_start(AiPage *p,AiJobType type,GPtrArray *marked) {
    if (p->busy || (!p->backend && type!=AI_REFRESH_CATALOG && type!=AI_LOAD_LOCAL)) {
        if (marked) g_ptr_array_unref(marked);
        return;
    }
    AiJob *j=g_new0(AiJob,1);
    j->page=p;j->type=type;j->marked=marked;
    ai_controls_busy(p,TRUE);
    if (type!=AI_LOAD_LOCAL) {
        ai_progress_begin(p,type==AI_REFRESH_CATALOG?ai_tr(AI_T_REFRESHING):ai_tr(AI_T_RUNNING));
        p->flush_source=g_timeout_add(130,ai_flush_log,p);
    } else ai_set_status(p,ai_tr(AI_T_LOADING));
    GThread *thread=g_thread_new("edpkg-appimage",ai_job_worker,j);
    g_thread_unref(thread);
}
static void ai_on_refresh(GtkButton *b,gpointer data){(void)b;ai_start(data,AI_REFRESH_CATALOG,NULL);}
static void ai_on_apply(GtkButton *b,gpointer data){
    (void)b;AiPage *p=data;
    if (p->busy || !p->backend) return;
    GPtrArray *marked=g_ptr_array_new_with_free_func(ai_marked_free);
    GHashTableIter it;gpointer value;g_hash_table_iter_init(&it,p->entries);
    while (g_hash_table_iter_next(&it,NULL,&value)) {
        AiEntry *a=value;
        if (!a->action) continue;
        AiMarked *m=g_new0(AiMarked,1);m->name=g_strdup(a->name);m->action=a->action;
        g_ptr_array_add(marked,m);
    }
    if (!marked->len) {
        GtkWidget *d=gtk_message_dialog_new(p->parent,GTK_DIALOG_MODAL,GTK_MESSAGE_INFO,GTK_BUTTONS_CLOSE,
                                             "%s",ai_tr(AI_T_NO_MARKS));
        gtk_dialog_run(GTK_DIALOG(d));gtk_widget_destroy(d);
        g_ptr_array_unref(marked);return;
    }
    AiJob check={0};check.marked=marked;
    if (ai_confirm(p,&check)) ai_start(p,AI_APPLY_CHANGES,marked);
    else g_ptr_array_unref(marked);
}
static gboolean ai_guard_close(GtkWidget *window,GdkEvent *event,gpointer data) {
    (void)window;(void)event;AiPage *p=data;
    /* Workers must not outlive the GTK tree they post results to. */
    if (p->busy) {ai_set_status(p,ai_tr(AI_T_WAIT));return TRUE;}
    return FALSE;
}
static GtkWidget *ai_button(const gchar *icon,const gchar *label) {
    GtkWidget *b=gtk_button_new();GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);
    GtkWidget *img=gtk_image_new_from_icon_name(icon,GTK_ICON_SIZE_BUTTON);
    GtkWidget *txt=gtk_label_new(label);
    gtk_box_pack_start(GTK_BOX(row),img,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(row),txt,FALSE,FALSE,0);
    gtk_container_add(GTK_CONTAINER(b),row);return b;
}
static void ai_add_column(GtkTreeView *view,const gchar *label,gint key,gint width) {
    GtkCellRenderer *renderer=gtk_cell_renderer_text_new();
    g_object_set(renderer,"ellipsize",PANGO_ELLIPSIZE_END,NULL);
    GtkTreeViewColumn *col=gtk_tree_view_column_new_with_attributes(label,renderer,"text",key,NULL);
    gtk_tree_view_column_set_resizable(col,TRUE);
    gtk_tree_view_column_set_sizing(col,GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col,width);
    gtk_tree_view_append_column(view,col);
}
/* Each AM catalogue entry gets Essora's generic AppImage icon next to its name.
 * A single pixbuf is shared by the renderer across every row: no per-row image
 * allocation, no filesystem access during scrolling, filtering or refresh. */
static void ai_add_application_column(GtkTreeView *view, const gchar *label,
                                      gint key, gint width, GdkPixbuf *icon) {
    GtkTreeViewColumn *col=gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col,label);
    GtkCellRenderer *image=gtk_cell_renderer_pixbuf_new();
    gtk_cell_renderer_set_fixed_size(image,32,30);
    if (icon) g_object_set(image,"pixbuf",icon,NULL);
    gtk_tree_view_column_pack_start(col,image,FALSE);
    GtkCellRenderer *text=gtk_cell_renderer_text_new();
    g_object_set(text,"ellipsize",PANGO_ELLIPSIZE_END,NULL);
    gtk_tree_view_column_pack_start(col,text,TRUE);
    gtk_tree_view_column_add_attribute(col,text,"text",key);
    gtk_tree_view_column_set_resizable(col,TRUE);
    gtk_tree_view_column_set_sizing(col,GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col,width);
    gtk_tree_view_append_column(view,col);
}
/* Display the installed-package indicator used in the DEB tab.
 * AM recipes do not provide a reliable outdated flag: avoid fake orange rows. */
static void ai_add_status_column(GtkTreeView *view) {
    GtkCellRenderer *r=gtk_cell_renderer_pixbuf_new();
    g_object_set(r,"xalign",0.5,NULL);
    gtk_cell_renderer_set_fixed_size(r,28,28);
    GtkTreeViewColumn *c=gtk_tree_view_column_new_with_attributes("",r,
        "pixbuf",AI_COL_STATE_ICON,NULL);
    gtk_tree_view_column_set_sizing(c,GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c,34);
    gtk_tree_view_append_column(view,c);
}

GtkWidget *appimage_page_new(GtkWindow *parent,gboolean is_root) {
    ai_select_language();
    AiPage *p=g_new0(AiPage,1);
    p->parent=parent;p->is_root=is_root;p->backend=ai_find_backend(is_root);
    p->cache_path=is_root ? g_strdup(AI_CACHE) :
      g_build_filename(g_get_user_cache_dir(),"edpkg-gui","appimage-catalog-v1.tsv",NULL);
    p->entries=ai_entries_new();p->filter=AI_SHOW_INSTALLED;
    /* Two small resources loaded on entering AppImage, never per row. */
    p->icon_installed=gdk_pixbuf_new_from_file_at_scale(AI_INSTALLED_ICON,24,24,TRUE,NULL);
    p->icon_appimage=gdk_pixbuf_new_from_file_at_scale(AI_ICON,28,28,TRUE,NULL);
    p->pending_log=g_string_new("");g_mutex_init(&p->log_mutex);
    p->root=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    GtkWidget *toolbar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,7);
    gtk_widget_set_name(toolbar,"toolbar");
    p->btn_refresh=ai_button("view-refresh",ai_tr(AI_T_REFRESH));
    p->btn_mark=ai_button("software-update-available",ai_tr(AI_T_MARK_UPDATES));
    p->btn_apply=ai_button("emblem-default",ai_tr(AI_T_APPLY));
    gtk_widget_set_name(p->btn_apply,"apply");
    gtk_box_pack_start(GTK_BOX(toolbar),p->btn_refresh,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(toolbar),p->btn_mark,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(toolbar),p->btn_apply,FALSE,FALSE,0);
    p->search=gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->search),ai_tr(AI_T_SEARCH));
    gtk_widget_set_size_request(p->search,230,-1);
    gtk_box_pack_end(GTK_BOX(toolbar),p->search,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(p->root),toolbar,FALSE,FALSE,0);
    g_signal_connect(p->btn_refresh,"clicked",G_CALLBACK(ai_on_refresh),p);
    g_signal_connect(p->btn_mark,"clicked",G_CALLBACK(ai_on_mark_updates),p);
    g_signal_connect(p->btn_apply,"clicked",G_CALLBACK(ai_on_apply),p);
    g_signal_connect(p->search,"changed",G_CALLBACK(ai_search_changed),p);
    p->notice=gtk_info_bar_new();
    gtk_widget_set_name(p->notice,"ai-notice");
    /* The global Essora CSS only sets a light text colour, which left the notice
     * (e.g. "AM is not installed...") unreadable on the theme's info-bar colour. */
    {
        GtkCssProvider *css=gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "#ai-notice, #ai-notice revealer, #ai-notice box {"
            " background-color:#17364d; background-image:none; border:0; box-shadow:none; }"
            "#ai-notice { border-bottom:2px solid #236648; }"
            "#ai-notice label { color:#e4edf3; }",-1,NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION+1);
        g_object_unref(css);
    }
    gtk_widget_set_no_show_all(p->notice,TRUE);
    gtk_info_bar_set_message_type(GTK_INFO_BAR(p->notice),GTK_MESSAGE_INFO);
    GtkWidget *msg=gtk_label_new("");gtk_label_set_xalign(GTK_LABEL(msg),0);
    gtk_label_set_line_wrap(GTK_LABEL(msg),TRUE);
    gtk_widget_set_margin_start(msg,8);gtk_widget_set_margin_end(msg,8);
    /* set_no_show_all() on the bar also stops show_all() from reaching this label,
     * so the notice rendered as an empty strip.  Show it explicitly; make it
     * selectable so the install command can be copied. */
    gtk_label_set_selectable(GTK_LABEL(msg),TRUE);
    gtk_widget_set_can_focus(msg,FALSE);   /* otherwise GTK highlights the whole text */
    gtk_widget_show(msg);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(p->notice))),msg);
    g_object_set_data(G_OBJECT(p->notice),"ai-label",msg);
    gtk_box_pack_start(GTK_BOX(p->root),p->notice,FALSE,FALSE,0);
    GtkWidget *pan=gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(p->root),pan,TRUE,TRUE,0);
    GtkWidget *side=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);
    gtk_widget_set_name(side,"side");gtk_widget_set_size_request(side,170,-1);
    /* Use the supplied high-resolution Essora artwork only when opening this tab. */
    GdkPixbuf *logo=gdk_pixbuf_new_from_file_at_scale(
        "/usr/lib/edpkg-gui/essora-appimage-256.png",68,68,TRUE,NULL);
    if (logo) {
        GtkWidget *brand=gtk_image_new_from_pixbuf(logo);
        gtk_widget_set_margin_top(brand,8);
        gtk_widget_set_margin_bottom(brand,8);
        gtk_box_pack_start(GTK_BOX(side),brand,FALSE,FALSE,0);
        g_object_unref(logo);
    }
    AiKey names[3]={AI_T_INSTALLED,AI_T_ALL,AI_T_AVAILABLE};
    for (int i=0;i<3;i++) {
        GtkWidget *b=gtk_toggle_button_new_with_label(ai_tr(names[i]));
        p->filters[i]=b;
        g_object_set_data(G_OBJECT(b),"ai-filter",GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(side),b,FALSE,FALSE,0);
        g_signal_connect(b,"clicked",G_CALLBACK(ai_filter_clicked),p);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->filters[0]),TRUE);
    p->filter_chosen=FALSE;   /* set_active() above emitted "clicked"; that was not the user */
    gtk_paned_pack1(GTK_PANED(pan),side,FALSE,FALSE);
    GtkWidget *right=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    gtk_paned_pack2(GTK_PANED(pan),right,TRUE,FALSE);
    p->store=gtk_list_store_new(AI_N_COLUMNS,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,
                                G_TYPE_STRING,GDK_TYPE_PIXBUF);
    p->tree=gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->store));
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(p->tree),TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(p->tree),FALSE);
    ai_add_column(GTK_TREE_VIEW(p->tree),ai_tr(AI_T_ACTION),AI_COL_ACTION,105);
    ai_add_status_column(GTK_TREE_VIEW(p->tree));
    ai_add_application_column(GTK_TREE_VIEW(p->tree),ai_tr(AI_T_NAME),
                              AI_COL_NAME,258,p->icon_appimage);
    ai_add_column(GTK_TREE_VIEW(p->tree),ai_tr(AI_T_VERSION),AI_COL_VERSION,150);
    ai_add_column(GTK_TREE_VIEW(p->tree),ai_tr(AI_T_DESCRIPTION),AI_COL_DESCRIPTION,300);
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll),p->tree);
    gtk_box_pack_start(GTK_BOX(right),scroll,TRUE,TRUE,0);
    GtkTreeSelection *sel=gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree));
    gtk_tree_selection_set_mode(sel,GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel,"changed",G_CALLBACK(ai_on_selection),p);
    GtkWidget *detail=gtk_box_new(GTK_ORIENTATION_VERTICAL,5);
    gtk_widget_set_name(detail,"details");
    p->detail_name=gtk_label_new(ai_tr(AI_T_SELECT));
    gtk_widget_set_name(p->detail_name,"details-title");
    gtk_label_set_xalign(GTK_LABEL(p->detail_name),0);
    p->detail_desc=gtk_label_new("");gtk_label_set_xalign(GTK_LABEL(p->detail_desc),0);
    gtk_label_set_line_wrap(GTK_LABEL(p->detail_desc),TRUE);
    gtk_box_pack_start(GTK_BOX(detail),p->detail_name,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(detail),p->detail_desc,FALSE,FALSE,0);
    GtkWidget *actions=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    p->btn_install=gtk_button_new_with_label(ai_tr(AI_T_INSTALL));
    p->btn_remove=gtk_button_new_with_label(ai_tr(AI_T_REMOVE));
    p->btn_update=gtk_button_new_with_label(ai_tr(AI_T_UPDATE));
    p->btn_clear=gtk_button_new_with_label(ai_tr(AI_T_CLEAR));
    gtk_box_pack_start(GTK_BOX(actions),p->btn_install,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(actions),p->btn_remove,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(actions),p->btn_update,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(actions),p->btn_clear,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(detail),actions,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(right),detail,FALSE,FALSE,0);
    g_signal_connect(p->btn_install,"clicked",G_CALLBACK(ai_on_install),p);
    g_signal_connect(p->btn_remove,"clicked",G_CALLBACK(ai_on_remove),p);
    g_signal_connect(p->btn_update,"clicked",G_CALLBACK(ai_on_update),p);
    g_signal_connect(p->btn_clear,"clicked",G_CALLBACK(ai_on_clear),p);
    /* No network checks, catalog parsing or AM subprocesses in page constructor. */
    p->status=gtk_label_new(ai_tr(AI_T_READY));
    gtk_widget_set_name(p->status,"statusbar");
    gtk_label_set_xalign(GTK_LABEL(p->status),0);
    gtk_box_pack_start(GTK_BOX(p->root),p->status,FALSE,FALSE,0);
    ai_notice_update(p);
    ai_update_details(p);
    gtk_widget_set_sensitive(p->btn_refresh,TRUE);
    gtk_widget_set_sensitive(p->btn_mark,FALSE);
    gtk_widget_set_sensitive(p->btn_apply,FALSE);
    g_signal_connect(parent,"delete-event",G_CALLBACK(ai_guard_close),p);
    /* Load saved catalog even without AM; never download on startup. */
    ai_start(p,AI_LOAD_LOCAL,NULL);
    return p->root;
}
