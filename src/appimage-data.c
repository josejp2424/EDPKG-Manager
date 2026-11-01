#include "appimage-data.h"
#include <string.h>

/* AM colors names even when list output is redirected. */
static void ai_remove_ansi(gchar *line) {
    gchar *src=line, *dst=line;
    while (*src) {
        if (src[0]=='\033' && src[1]=='[') {
            src+=2;
            while (*src && ((*src>='0' && *src<='9') || *src==';')) src++;
            if (*src=='m') {src++;continue;}
        }
        *dst++=*src++;
    }
    *dst=0;
}
static void ai_entry_free(gpointer value) {
    AiEntry *a = value;
    g_free(a->name); g_free(a->version); g_free(a->description); g_free(a);
}
GHashTable *ai_entries_new(void) {
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ai_entry_free);
}
gboolean ai_valid_name(const gchar *name) {
    if (!name || !g_ascii_isalnum((guchar)name[0]) || strlen(name) > 180) return FALSE;
    for (const guchar *s = (const guchar *)name; *s; s++)
        if (!g_ascii_isalnum(*s) && *s != '.' && *s != '_' && *s != '+' && *s != '-') return FALSE;
    return TRUE;
}
AiEntry *ai_entry_get(GHashTable *entries, const gchar *name) {
    if (!ai_valid_name(name)) return NULL;
    AiEntry *a = g_hash_table_lookup(entries, name);
    if (!a) {
        a = g_new0(AiEntry, 1); a->name = g_strdup(name);
        a->version = g_strdup(""); a->description = g_strdup("");
        g_hash_table_insert(entries, g_strdup(name), a);
    }
    return a;
}
/* Installed-application rows printed by `am -fi --byname`.
 *
 * AM 10.x (checked against 10.6-6) prints a table whose data rows look like
 *     _ ◆name | DB | VERSION | TYPE | SIZE
 * The first character of the row (`_`, `-`...) varies and there is NO space
 * after the ◆ marker.  Older AM builds printed `◆ name | VERSION | TYPE | SIZE`.
 * Both layouts are accepted and chosen by column count.  TYPE must start with
 * "appimage" (it may carry the sandbox/FUSE marks appimage🔒 / appimage*), so
 * portable binaries, scripts and libraries that AM also lists are skipped. */
guint ai_parse_installed(GHashTable *entries, const gchar *output) {
    guint count = 0;
    if (!output) return 0;
    gchar **lines = g_strsplit(output, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        ai_remove_ansi(lines[i]);
        gchar *marker = strstr(lines[i], "◆");
        if (!marker) continue;
        gchar *clean = g_strstrip(marker + strlen("◆"));
        gchar **fields = g_strsplit(clean, "|", 6);
        guint n = g_strv_length(fields);
        if (n >= 3) {
            /* name | db | version | type | size   (AM 10.x)
             * name | version | type [| size]      (older layout) */
            guint version_col = n >= 5 ? 2 : 1;
            gchar *name = g_strstrip(fields[0]);
            gchar *version = g_strstrip(fields[version_col]);
            gchar *kind = g_ascii_strdown(g_strstrip(fields[version_col + 1]), -1);
            if (g_str_has_prefix(kind, "appimage")) {
                AiEntry *a = ai_entry_get(entries, name);
                if (a && !a->installed) {
                    a->installed = TRUE;
                    g_free(a->version); a->version = g_strdup(version);
                    count++;
                }
            }
            g_free(kind);
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    return count;
}
/* AM list output contains `◆ id : description` (sometimes only `◆ id`).
 * Never allow terminal formatting or untrusted punctuation to become argv. */
guint ai_parse_catalog(GHashTable *entries, const gchar *output) {
    guint found = 0;
    if (!output) return 0;
    gchar **lines = g_strsplit(output, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        ai_remove_ansi(lines[i]);
        gchar *line = g_strstrip(lines[i]);
        if (!g_str_has_prefix(line, "◆")) continue;
        line = g_strstrip(line + strlen("◆"));
        gchar *cut = strchr(line, ':');
        if (cut) *cut++ = '\0';
        gchar *name = g_strstrip(line);
        /* Names are unique AM package identifiers, no whitespace, URLs or paths. */
        AiEntry *a = ai_entry_get(entries, name);
        if (!a) continue;
        if (cut && *g_strstrip(cut) && !*a->description) {
            gchar *desc = g_strstrip(cut);
            if (g_utf8_validate(desc, -1, NULL)) {
                g_free(a->description);
                a->description = g_strndup(desc, 4096);
            }
        }
        found++;
    }
    g_strfreev(lines);
    return found;
}
guint ai_parse_catalog_cache(GHashTable *entries, const gchar *output) {
    guint found = 0;
    if (!output) return 0;
    gchar **lines = g_strsplit(output, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        gchar *tab = strchr(lines[i], '\t');
        if (!tab) continue;
        *tab++ = '\0';
        AiEntry *a = ai_entry_get(entries, lines[i]);
        if (!a) continue;
        if (g_utf8_validate(tab, -1, NULL)) {
            g_free(a->description);
            a->description = g_strndup(tab, 4096);
        }
        found++;
    }
    g_strfreev(lines);
    return found;
}
gchar *ai_create_cache(GHashTable *entries) {
    GString *out = g_string_new("");
    GHashTableIter it; gpointer val;
    g_hash_table_iter_init(&it, entries);
    while (g_hash_table_iter_next(&it, NULL, &val)) {
        AiEntry *a = val;
        if (ai_valid_name(a->name)) {
            gchar *desc = g_strdup(a->description);
            for (gchar *c = desc; *c; c++) if (*c == '\n' || *c == '\t' || *c == '\r') *c = ' ';
            g_string_append_printf(out, "%s\t%s\n", a->name, desc);
            g_free(desc);
        }
    }
    return g_string_free(out, FALSE);
}
