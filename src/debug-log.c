#define _GNU_SOURCE
#include "debug-log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEBUG_ROTATE_BYTES (2 * 1024 * 1024)

static GMutex log_lock;
static gboolean debug_enabled = FALSE;

static const char *safe(const char *s) { return s ? s : ""; }

gboolean eapt_debug_enabled(void) { return debug_enabled; }

static gboolean prepare_debug_dir(void) {
    if (g_mkdir_with_parents(EAPT_DEBUG_LOG_DIR, 0700) != 0 && errno != EEXIST)
        return FALSE;
    (void)chmod(EAPT_DEBUG_LOG_DIR, 0700);
    struct stat st;
    if (lstat(EAPT_DEBUG_LOG_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) return FALSE;
    if (st.st_uid != geteuid()) return FALSE;
    return TRUE;
}

static void rotate_if_needed(void) {
    if (!debug_enabled) return;
    struct stat st;
    if (lstat(EAPT_DEBUG_LOG_PATH, &st) != 0) return;
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) return;
    if (st.st_size <= DEBUG_ROTATE_BYTES) return;
    char old[256];
    g_snprintf(old, sizeof(old), "%s.old", EAPT_DEBUG_LOG_PATH);
    (void)unlink(old);
    (void)rename(EAPT_DEBUG_LOG_PATH, old);
}

void eapt_debug_log(const char *fmt, ...) {
    if (!debug_enabled) return;
    g_mutex_lock(&log_lock);
    rotate_if_needed();
    int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(EAPT_DEBUG_LOG_PATH, flags, 0600);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid()) {
            GDateTime *now = g_date_time_new_now_local();
            char *stamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S.%f");
            GString *line = g_string_new(NULL);
            g_string_append_printf(line, "%s pid=%ld tid=%p ", stamp, (long)getpid(), (void *)g_thread_self());
            va_list ap;
            va_start(ap, fmt);
            char *msg = g_strdup_vprintf(fmt, ap);
            va_end(ap);
            g_string_append(line, msg ? msg : "");
            if (!line->len || line->str[line->len - 1] != '\n') g_string_append_c(line, '\n');
            size_t off = 0;
            while (off < line->len) {
                ssize_t n = write(fd, line->str + off, line->len - off);
                if (n > 0) { off += (size_t)n; continue; }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            g_free(msg);
            g_string_free(line, TRUE);
            g_free(stamp);
            g_date_time_unref(now);
        }
        close(fd);
    }
    g_mutex_unlock(&log_lock);
}

static void append_window_state(GString *s, GtkWidget *widget) {
    GtkAllocation a = {0};
    gtk_widget_get_allocation(widget, &a);
    g_string_append_printf(s,
        " widget=%p type=%s visible=%d mapped=%d realized=%d sensitive=%d alloc=%dx%d+%d+%d",
        (void *)widget, G_OBJECT_TYPE_NAME(widget),
        gtk_widget_get_visible(widget), gtk_widget_get_mapped(widget),
        gtk_widget_get_realized(widget), gtk_widget_get_sensitive(widget),
        a.width, a.height, a.x, a.y);
    if (GTK_IS_WINDOW(widget)) {
        GtkWindow *w = GTK_WINDOW(widget);
        GtkWindow *tr = gtk_window_get_transient_for(w);
        g_string_append_printf(s,
            " title=\"%s\" active=%d modal=%d transient=%p destroy_parent=%d deletable=%d decorated=%d type_hint=%d",
            safe(gtk_window_get_title(w)), gtk_window_is_active(w), gtk_window_get_modal(w), (void *)tr,
            gtk_window_get_destroy_with_parent(w), gtk_window_get_deletable(w),
            gtk_window_get_decorated(w), (int)gtk_window_get_type_hint(w));
    }
}

void eapt_debug_widget(const char *tag, GtkWidget *widget) {
    if (!debug_enabled) return;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "WIDGET %s", safe(tag));
    if (widget) append_window_state(s, widget); else g_string_append(s, " widget=(nil)");
    eapt_debug_log("%s", s->str);
    g_string_free(s, TRUE);
}

void eapt_debug_toplevels(const char *tag) {
    if (!debug_enabled) return;
    GList *list = gtk_window_list_toplevels();
    eapt_debug_log("TOPLEVELS %s count=%u", safe(tag), g_list_length(list));
    guint i = 0;
    for (GList *l = list; l; l = l->next, i++) {
        GString *s = g_string_new(NULL);
        g_string_append_printf(s, "TOPLEVEL[%u] %s", i, safe(tag));
        append_window_state(s, GTK_WIDGET(l->data));
        eapt_debug_log("%s", s->str);
        g_string_free(s, TRUE);
    }
    g_list_free(list);
}

static gboolean debug_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
    if (!debug_enabled || !event) return FALSE;
    if (event->type != GDK_FOCUS_CHANGE && event->type != GDK_MAP && event->type != GDK_UNMAP &&
        event->type != GDK_WINDOW_STATE && event->type != GDK_GRAB_BROKEN && event->type != GDK_DELETE)
        return FALSE;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "EVENT %s type=%d", safe((const char *)data), (int)event->type);
    append_window_state(s, widget);
    eapt_debug_log("%s", s->str);
    g_string_free(s, TRUE);
    return FALSE;
}

static void debug_destroy(GtkWidget *widget, gpointer data) {
    if (!debug_enabled) return;
    eapt_debug_widget(data ? (const char *)data : "DESTROY", widget);
}

void eapt_debug_watch_window(GtkWidget *widget, const char *tag) {
    if (!debug_enabled || !widget) return;
    g_signal_connect(widget, "event", G_CALLBACK(debug_event), (gpointer)tag);
    g_signal_connect(widget, "destroy", G_CALLBACK(debug_destroy), (gpointer)tag);
    eapt_debug_widget(tag, widget);
}

typedef struct { char *tag; } DelayedDump;
static gboolean delayed_dump_cb(gpointer data) {
    DelayedDump *d = data;
    if (debug_enabled) eapt_debug_toplevels(d->tag);
    g_free(d->tag); g_free(d);
    return G_SOURCE_REMOVE;
}

void eapt_debug_schedule_toplevels(const char *tag, guint delay_ms) {
    if (!debug_enabled) return;
    DelayedDump *d = g_new0(DelayedDump, 1);
    d->tag = g_strdup(tag ? tag : "delayed");
    g_timeout_add(delay_ms, delayed_dump_cb, d);
}

void eapt_debug_session_start(int argc, char **argv) {
    const char *env = g_getenv("EDPKG_GUI_DEBUG");
    debug_enabled = env && *env && strcmp(env, "0") != 0;
    if (!debug_enabled) return;
    if (!prepare_debug_dir()) { debug_enabled = FALSE; return; }
    GString *cmd = g_string_new(NULL);
    for (int i = 0; i < argc; i++) {
        if (i) g_string_append_c(cmd, ' ');
        char *q = g_shell_quote(argv[i] ? argv[i] : "");
        g_string_append(cmd, q); g_free(q);
    }
    eapt_debug_log("========== SESSION START ==========");
    eapt_debug_log("process ppid=%ld uid=%ld euid=%ld gid=%ld egid=%ld cmd=%s",
        (long)getppid(), (long)getuid(), (long)geteuid(), (long)getgid(), (long)getegid(), cmd->str);
    g_string_free(cmd, TRUE);
}

void eapt_debug_session_end(void) {
    if (!debug_enabled) return;
    eapt_debug_log("========== SESSION END ==========");
}
