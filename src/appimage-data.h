#ifndef ESSORA_APPIMAGE_DATA_H
#define ESSORA_APPIMAGE_DATA_H
#include <glib.h>

typedef enum { AI_NONE, AI_INSTALL, AI_REMOVE, AI_UPDATE } AiAction;
typedef struct {
    gchar *name;
    gchar *version;
    gchar *description;
    gboolean installed;
    AiAction action;
} AiEntry;

GHashTable *ai_entries_new(void);
AiEntry *ai_entry_get(GHashTable *entries, const gchar *name);
gboolean ai_valid_name(const gchar *name);
guint ai_parse_installed(GHashTable *entries, const gchar *output);
guint ai_parse_catalog(GHashTable *entries, const gchar *output);
guint ai_parse_catalog_cache(GHashTable *entries, const gchar *output);
gchar *ai_create_cache(GHashTable *entries);
#endif
