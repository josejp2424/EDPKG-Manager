#!/bin/sh
set -eu
SRC="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/src/flatpak-page.c"
grep -Fq 'GHashTable *pixbuf_cache' "$SRC"
grep -Fq 'g_hash_table_lookup(p->pixbuf_cache, app_id)' "$SRC"
grep -Fq 'g_hash_table_replace(p->pixbuf_cache, g_strdup(app_id), g_object_ref(pb))' "$SRC"
grep -Fq 'g_hash_table_remove_all(p->pixbuf_cache)' "$SRC"
echo 'PASS: Flatpak pixbufs are decoded once per icon-cache generation'
