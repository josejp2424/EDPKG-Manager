#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$ROOT/src/edpkg-gui.c"
grep -Fq 'static void overlay_apt_policy_candidates' "$SRC"
grep -Fq 'APT_BIN " list 2>/dev/null"' "$SRC"
grep -Fq 'arch_is_visible(a,arch)' "$SRC"
python3 - "$SRC" <<'PY'
import sys
s=open(sys.argv[1],encoding='utf-8').read()
w0=s.index('static gpointer load_worker')
w1=s.index('static void start_load_async',w0)
w=s[w0:w1]
# Candidate overlay belongs only to the explicit Refresh branch.
assert w.count('overlay_apt_policy_candidates(&tmp);') == 1
pos=w.index('overlay_apt_policy_candidates(&tmp);')
force=w.index('/* Explicit Refresh owns the expensive repository-catalog rebuild. */')
assert pos > force
print('PASS: APT candidate versions are overlaid from APT policy only during explicit Refresh')
PY
