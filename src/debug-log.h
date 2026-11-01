#ifndef EDPKG_GUI_DEBUG_LOG_H
#define EDPKG_GUI_DEBUG_LOG_H

#include <gtk/gtk.h>

#define EAPT_DEBUG_LOG_DIR "/run/edpkg-gui"
#define EAPT_DEBUG_LOG_PATH EAPT_DEBUG_LOG_DIR "/edpkg-gui.log"

void eapt_debug_session_start(int argc, char **argv);
void eapt_debug_session_end(void);
void eapt_debug_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void eapt_debug_widget(const char *tag, GtkWidget *widget);
void eapt_debug_toplevels(const char *tag);
void eapt_debug_watch_window(GtkWidget *widget, const char *tag);
void eapt_debug_schedule_toplevels(const char *tag, guint delay_ms);
gboolean eapt_debug_enabled(void);

#endif
