#!/bin/sh
# 0.1.36: AppImage tab against REAL AM behaviour (AM 10.6-6 output captured in
# tests/fixtures) instead of the format earlier versions assumed.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
command -v pkg-config >/dev/null && pkg-config --exists gio-2.0 || { echo "glib/gio dev files required" >&2; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 1) Parser: real AM 10.x rows, legacy rows, non-AppImage rows, hostile names.
cat > "$TMP/parse.c" <<'EOF'
#include "appimage-data.h"
#include <assert.h>
#include <stdio.h>
static AiEntry *get(GHashTable *e,const char *n){return g_hash_table_lookup(e,n);}
int main(int argc,char **argv){ (void)argc;
  gchar *real=NULL; assert(g_file_get_contents(argv[1],&real,NULL,NULL));
  GHashTable *e=ai_entries_new();
  assert(ai_parse_installed(e,real)==2);
  assert(get(e,"fakeone") && get(e,"fakeone")->installed);
  assert(g_str_equal(get(e,"fakeone")->version,"1.2.3"));
  assert(get(e,"faketwo") && get(e,"faketwo")->installed);
  assert(!get(e,"am"));                       /* bash-script row is not an AppImage */
  g_hash_table_unref(e);
  /* sandbox/FUSE marks keep the AppImage type */
  e=ai_entries_new();
  assert(ai_parse_installed(e,
    " _ ◆box    | am | 2.0 | appimage🔒  | 1 MiB\n"
    " _ ◆old    | am | 3.1 | appimage*   | 1 MiB\n"
    " _ ◆both   | am | 4.2 | appimage🔒* | 1 MiB\n"
    " _ ◆tool   | am | 1.0 | static-binary | 1 MiB\n"
    " _ ◆lib    | am | 1.0 | library     | 1 MiB\n")==3);
  assert(get(e,"box")&&get(e,"old")&&get(e,"both")&&!get(e,"tool")&&!get(e,"lib"));
  g_hash_table_unref(e);
  /* legacy layout: name | version | type | size */
  e=ai_entries_new();
  assert(ai_parse_installed(e," ◆ krita | 5.2.2 | appimage | 300 MiB\n ◆ x | 1 | script | 1 KiB\n")==1);
  assert(g_str_equal(get(e,"krita")->version,"5.2.2"));
  g_hash_table_unref(e);
  /* argv-injection style names and junk never become entries */
  e=ai_entries_new();
  assert(ai_parse_installed(e," _ ◆-R | am | 1 | appimage | 1 KiB\n _ ◆../x | am | 1 | appimage | 1 KiB\n"
                             " _ ◆a b | am | 1 | appimage | 1 KiB\nno marker | am | 1 | appimage | 1\n")==0);
  assert(ai_parse_installed(e,NULL)==0 && ai_parse_installed(e,"")==0);
  g_hash_table_unref(e);
  puts("AM 10.6-6 installed-list parser: OK");
  return 0;}
EOF
gcc -Wall -Wextra -Werror -Isrc $(pkg-config --cflags glib-2.0) -o "$TMP/parse" "$TMP/parse.c" src/appimage-data.c $(pkg-config --libs glib-2.0)
"$TMP/parse" tests/fixtures/am-fi-10.6-6.txt

# 2) Real ai_exec / ai_exec_am against a fake `am` that reproduces AM's guard.
cat > "$TMP/am" <<'EOF'
#!/bin/sh
if [ "$(id -u)" = 0 ] && [ -n "${SUDO_USER:-$DOAS_USER}" ]; then
  echo 'Please do not use "sudo" to execute "am", try again.'; exit 1
fi
case "$1" in
  ok) echo "fine"; exit 0;;
  boom) echo " 💀 ERROR DURING INSTALLATION, REMOVED X!"; exit 0;;   # AM exits 0 here
  env) env | grep -E '^(SUDO_|DOAS_)' || echo "none"; exit 0;;
esac
EOF
chmod +x "$TMP/am"
cat > "$TMP/exec.c" <<'EOF'
#include <gio/gio.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { void *dialog; } AiPage;
static void ai_queue_text(AiPage *p,const gchar *t,gboolean s){(void)p;(void)t;(void)s;}
EOF
python3 - "$ROOT/src/appimage-page.c" >> "$TMP/exec.c" <<'PY'
import sys
s=open(sys.argv[1]).read()
def block(begin):
    st=s.index(begin); b=s.index('{',st); d=0
    for i in range(b,len(s)):
        d+=(s[i]=='{')-(s[i]=='}')
        if d==0: return s[st:i+1]
print(block('static gboolean ai_exec(')); print(block('static gboolean ai_exec_am('))
PY
cat >> "$TMP/exec.c" <<'EOF'
int main(int argc,char **argv){
  AiPage p={0}; gchar *out=NULL; (void)argc;
  g_setenv("SUDO_USER","josejp",TRUE); g_setenv("SUDO_UID","1000",TRUE);
  g_setenv("SUDO_GID","1000",TRUE); g_setenv("SUDO_COMMAND","/x",TRUE); g_setenv("DOAS_USER","josejp",TRUE);
  const gchar *ok[]={argv[1],"ok",NULL};
  assert(ai_exec(&p,ok,&out) && strstr(out,"fine")); g_free(out);
  const gchar *env[]={argv[1],"env",NULL};
  assert(ai_exec(&p,env,&out) && strstr(out,"none")); g_free(out);
  const gchar *boom[]={argv[1],"boom",NULL};
  assert(ai_exec(&p,boom,&out));  g_free(out);            /* raw exec: AM said 0 */
  assert(!ai_exec_am(&p,boom,&out) && strstr(out,"ERROR DURING")); g_free(out);
  assert(ai_exec_am(&p,ok,NULL));
  puts("ai_exec drops SUDO_*/DOAS_* and ai_exec_am catches AM exit-0 failures: OK");
  return 0;}
EOF
gcc -Wall -Wextra -o "$TMP/exec" "$TMP/exec.c" $(pkg-config --cflags --libs gio-2.0)
"$TMP/exec" "$TMP/am"

# 3) Source contracts that keep the fixes wired in.
python3 - <<'PY'
from pathlib import Path
ai=Path('src/appimage-page.c').read_text()
assert ai.count('ai_exec_am(p,args')==3
assert 'gboolean done=ai_exec_am(p,args,NULL);' in ai
assert 'gboolean local_ok=ai_exec_am(p,args,&installed);' in ai
assert 'ok=ai_exec(p,args,&catalog)' in ai          # catalog text must NOT use the skull check
assert 'm->action==AI_INSTALL && !present' in ai and 'm->action==AI_REMOVE && present' in ai
print('AppImage fixes wired into worker: OK')
PY
