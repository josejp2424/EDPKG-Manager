#!/bin/sh
set -eu
SRC="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
D="$SRC/src/edpkg-gui.c"
F="$SRC/src/flatpak-page.c"
grep -q 'a->filter_mode=F_INSTALLED' "$D"
grep -q 'p->filter_mode = FP_FILTER_INSTALLED' "$F"
grep -q 'load_local_async(a)' "$D"
grep -q 'j->local_only' "$D"
grep -q 'fp_load_local_async(p)' "$F"
grep -q 'j->local_only' "$F"
grep -q 'package_matches_filter' "$D"
grep -q 'fp_app_matches_filter' "$F"
# Local status must not invalidate the repository catalog cache.
! sed -n '/static gchar \*cache_signature/,/return ret/p' "$D" | grep -q 'checksum_meta(sum, status_file)'
echo 'instant startup contract: OK'
