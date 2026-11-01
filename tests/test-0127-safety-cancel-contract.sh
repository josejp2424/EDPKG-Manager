#!/bin/sh
set -eu
SRC=src/edpkg-gui.c
FP=src/flatpak-page.c

grep -q '#define APP_VERSION "0.1.40"' "$SRC"
# One-character search is skipped only while typing forward, not while deleting.
grep -q 'glong prev=a->search_folded?g_utf8_strlen(a->search_folded,-1):0;' "$SRC"
grep -q 'if(n==1&&prev<=1){g_free(q);return G_SOURCE_REMOVE;}' "$SRC"
grep -q 'glong prev=p->search_folded?g_utf8_strlen(p->search_folded,-1):0;' "$FP"
grep -q 'if(n==1&&prev<=1){g_free(q);return G_SOURCE_REMOVE;}' "$FP"

# Simulations are asynchronous/cancellable and publish a real PID.
grep -q 'static gchar \*capture_guard_argv' "$SRC"
grep -q 'G_SPAWN_DO_NOT_REAP_CHILD' "$SRC"
grep -q 'g_atomic_int_set(&a->active_child_pid,(gint)pid);' "$SRC"
grep -q 'SIM_CHECK_UNSUPPORTED' "$SRC"
grep -q 'SIM_CHECK_UNRECOGNIZED' "$SRC"
grep -q 'recognized==0' "$SRC"
grep -q 'installed EDPKG does not advertise --simulate/--dry-run support' "$SRC"
grep -q 'char \*av\[\]={(char\*)edpkg,"--help",NULL};' "$SRC"

# Cancellation has its own result, resynchronizes local state and is not called failure.
grep -q 'f->cancelled?tr(T_CANCELLED)' "$SRC"
grep -q 'if((rc==0||cancelled)&&(j->type==JOB_APPLY||j->type==JOB_FIX))' "$SRC"
grep -q 'if(g_atomic_int_get(&a->cancel_requested))return 130;' "$SRC"

# Closing during catalog loading is allowed; only a live transaction blocks close.
python3 - "$SRC" <<'PY2'
import re,sys
s=open(sys.argv[1],encoding='utf-8').read()
m=re.search(r'static gboolean main_delete_event\(.*?\n\}',s,re.S)
assert m
b=m.group(0)
assert 'return a->busy;' in b
assert 'a->loading' not in b
PY2

# Once package mutation begins Cancel is disabled instead of SIGINT'ing dpkg.
grep -q 'line_enters_package_mutation' "$SRC"
grep -q 'cancel_locked' "$SRC"
grep -q 'static void cancel_phase_package_command' "$SRC"
grep -q 'static void cancel_phase_safe' "$SRC"

echo '0.1.27 safety/cancel contract: OK'
