#!/bin/sh
set -eu
D="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/src/edpkg-gui.c"
F="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/src/flatpak-page.c"
python3 - "$D" "$F" <<'PYTEST'
import re,sys
s=open(sys.argv[1],encoding='utf-8').read()
f=open(sys.argv[2],encoding='utf-8').read()
# The modal must keep its buttons in the dialog action area, not within the list.
start=s.index('static gboolean confirm_deb_changes')
end=s.index('static void on_apply',start)
confirm=s[start:end]
assert 'gtk_dialog_new_with_buttons' in confirm
assert 'gtk_scrolled_window_new' in confirm
assert 'gtk_text_buffer_set_text' in confirm
assert 'gdk_monitor_get_workarea' in confirm
assert 'gtk_dialog_run' in confirm
assert 'GTK_RESPONSE_OK' in confirm
assert 'gtk_message_dialog_format_secondary_text' not in s[s.index('static void on_apply'):s.index('static void on_fix')]
# Local-only first paint remains intact and refresh is explicit.
assert 'load_local_async(a)' in s and 'fp_load_local_async(p)' in f
for text in ('T_REPO_NOTICE','T_REFRESHING_REPOS','a->repo_refreshed','gtk_info_bar_new()'):
    assert text in s,text
for text in ('FT_REPO_NOTICE','FT_REFRESHING','p->repo_refreshed','gtk_info_bar_new()'):
    assert text in f,text
assert 'if(a->repo_refreshed)gtk_widget_hide(a->repo_notice)' in s
assert 'if(p->repo_refreshed)gtk_widget_hide(p->repo_notice)' in f
assert 'a->repo_refreshed=TRUE' in s
assert 'p->repo_refreshed=TRUE' in f
assert 'Se están actualizando los repositorios…' in s
assert 'Se están actualizando los repositorios Flatpak…' in f
for src,begin,end,num in ((s,'static const Lang langs[]','static const Lang *L',48),
                           (f,'static const FpLang fp_langs[]','static const FpLang *FP_L',35)):
    sec=src[src.index(begin):src.index(end)]
    rows=re.findall(r'\{"([a-z]+)", \{(.*?)\}\}',sec,re.S)
    assert [x[0] for x in rows]==['en','es','fr','it','pt','ca','de','hu','ru','ja','zh','ar']
    for code,body in rows:
        strings=re.findall(r'"(?:\\.|[^"\\])*"',body)
        assert len(strings)==num,(code,len(strings),num)
print('0.1.32 UI, first startup and i18n contract: OK')
PYTEST
