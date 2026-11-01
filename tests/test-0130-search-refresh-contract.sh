#!/bin/sh
set -eu
SRC=src/edpkg-gui.c
FP=src/flatpak-page.c

grep -q '#define APP_VERSION "0.1.40"' "$SRC"

# Search is global after 2 characters and no longer requires clicking All.
grep -q 'a->filter_mode=F_ALL;' "$SRC"
grep -q 'p->filter_mode=FP_FILTER_ALL;' "$FP"
grep -q 'Search results span the complete catalog regardless of sidebar filter.' "$SRC"
grep -q 'Search results span the complete catalog regardless of sidebar filter.' "$FP"
grep -q 'g_timeout_add(120,search_refilter_idle,a)' "$SRC"
grep -q 'g_timeout_add(120, fp_search_idle, p)' "$FP"

# Explicit Refresh clears search and lands on the complete freshly rebuilt list.
grep -q 'if(j->force && j->catalog_loaded' "$SRC"
grep -q 'Refresh always lands on the complete freshly rebuilt catalog.' "$SRC"
grep -q 'A completed Refresh always shows the complete new catalog.' "$FP"
grep -q 'gtk_entry_set_text(GTK_ENTRY(a->search),"")' "$SRC"
grep -q 'gtk_entry_set_text(GTK_ENTRY(p->search),"")' "$FP"

# Flatpak Refresh has its own indeterminate wait dialog and translated Spanish text.
grep -q 'static void fp_refresh_progress_show' "$FP"
grep -q 'if(force_remote) fp_refresh_progress_show(p);' "$FP"
grep -q 'gtk_progress_bar_pulse(GTK_PROGRESS_BAR(p->progress_bar))' "$FP"
grep -q 'Se están actualizando los repositorios Flatpak…' "$FP"
grep -q 'FT_REFRESHING' "$FP"

# Every Flatpak language row must still have exactly FT_COUNT strings.
python3 - "$FP" <<'PY2'
import re,sys
s=open(sys.argv[1],encoding='utf-8').read()
# Count the actual translation array rows instead; all rows must match English.
sec=s[s.index('static const FpLang fp_langs[]'):s.index('static const FpLang *FP_L')]
rows=re.findall(r'\{"([a-z]+)", \{(.*?)\}\}',sec,re.S)
assert len(rows)==12, len(rows)
counts=[]
for code,body in rows:
    n=len(re.findall(r'"(?:\\.|[^"\\])*"',body))
    counts.append((code,n))
assert len({n for _,n in counts})==1, counts
assert counts[0][1]==35, counts
PY2

echo '0.1.30 global-search/refresh contract: OK'
