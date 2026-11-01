#!/bin/sh
set -eu
src="$(dirname "$0")/../src/edpkg-gui.c"
proto=$(grep -n '^static void progress_append(App \*a, const char \*text);$' "$src" | head -n1 | cut -d: -f1)
first_call=$(grep -n 'progress_append(a,j->error);' "$src" | head -n1 | cut -d: -f1)
defn=$(grep -n '^static void progress_append(App \*a,const char \*text){$' "$src" | head -n1 | cut -d: -f1)
[ -n "$proto" ] && [ -n "$first_call" ] && [ -n "$defn" ]
[ "$proto" -lt "$first_call" ]
[ "$first_call" -lt "$defn" ]
echo "progress_append declaration order: OK"
