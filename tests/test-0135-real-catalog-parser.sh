#!/bin/sh
set -eu
BASE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$BASE"
python3 - <<'PY'
from pathlib import Path
import subprocess,tempfile
src=Path('src/appimage-data.c').read_text()
def extract(sig):
 s=src.index(sig); b=src.index('{',s);depth=0
 for i in range(b,len(src)):
  depth+=(src[i]=='{')-(src[i]=='}')
  if depth==0: return src[s:i+1]
 raise ValueError(sig)
ansi=extract('static void ai_remove_ansi(')
parser=extract('guint ai_parse_catalog(')
header=r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
typedef char gchar;
typedef unsigned int guint;
typedef void GHashTable;
#define g_free free
typedef struct { char *name,*description; } AiEntry;
static AiEntry names[12000];static size_t n_names;
static char *g_strstrip(char *s){
 while (*s && isspace((unsigned char)*s))s++;
 size_t n=strlen(s); while(n && isspace((unsigned char)s[n-1]))s[--n]=0;
 return s;
}
static char **g_strsplit(const char *text,const char *delim,int max){
 (void)max;assert(!strcmp(delim,"\n"));
 size_t count=1;for(const char *p=text;*p;p++)if(*p=='\n')count++;
 char **out=calloc(count+1,sizeof(char*));size_t idx=0;
 const char *begin=text,*p=text;
 for(;;p++){
  if(*p=='\n' || !*p){out[idx++]=strndup(begin,(size_t)(p-begin));begin=p+1;}
  if(!*p)break;
 }
 return out;
}
static void g_strfreev(char **v){for(size_t i=0;v[i];i++)free(v[i]);free(v);}
static int g_str_has_prefix(const char *a,const char *b){return strncmp(a,b,strlen(b))==0;}
static int g_utf8_validate(const char *a,int n,const char **invalid){(void)a;(void)n;(void)invalid;return 1;}
static char *g_strndup(const char *a,size_t n){return strndup(a,n);}
static AiEntry *ai_entry_get(void *entries,const char *name){
 (void)entries;
 if(!name || !isalnum((unsigned char)*name) || strlen(name)>180)return NULL;
 for(const char *p=name;*p;p++)if(!isalnum((unsigned char)*p) && *p!='-' && *p!='.' && *p!='+' && *p!='_')return NULL;
 for(size_t i=0;i<n_names;i++)if(!strcmp(names[i].name,name))return &names[i];
 if(n_names>=12000)abort(); AiEntry *item=&names[n_names++];item->name=strdup(name);item->description=strdup("");return item;
}
static void reset(void){for(size_t i=0;i<n_names;i++){free(names[i].name);free(names[i].description);}n_names=0;}
'''.replace('"\\\\n"','"\\n"')
main=r'''
int main(void){
 const char *sample=
 "◆ 0ad : FOSS historical Real Time Strategy, RTS game of ancient warfare.\\n"
 "◆ 0ad-prerelease : FOSS historical Real Time Strategy (Pre-release).\\n"
 "◆ 0x808 : Drum machine & synth workstation\\n"
 "◆ abiword : Unofficial. A light and free word processing software.\\n"
 "<html>error</html>\\n"
 "◆ malicious/name : must never appear\\n";
 char *real=strdup(sample);
 for(char *p=real;*p;p++){if(*p=='\\' && p[1]=='n'){*p='\n';memmove(p+1,p+2,strlen(p+2)+1);}}
 assert(ai_parse_catalog(NULL,real)==4);
 assert(n_names==4 && !strcmp(names[3].name,"abiword"));
 assert(strstr(names[0].description,"Strategy"));
 reset();free(real);
 assert(ai_parse_catalog(NULL,"<html>GitHub rate limit</html>")==0);
 reset();
 size_t capacity=1200000;char *mass=calloc(capacity,1);size_t used=0;
 for(int i=0;i<8757;i++){
  int n=snprintf(mass+used,capacity-used,"◆ app%d : Official catalog sample %d\\n",i,i);
  used+=(size_t)n;
 }
 for(char *p=mass;*p;p++){if(*p=='\\' && p[1]=='n'){*p='\n';memmove(p+1,p+2,strlen(p+2)+1);}}
 assert(ai_parse_catalog(NULL,mass)==8757);
 assert(n_names==8757);
 reset();free(mass);
 puts("PASS: real AM catalog row format, invalid responses rejected, 8757-entry parser");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as td:
 c=Path(td)/'catalog.c'; exe=Path(td)/'catalog';c.write_text(header+'\n'+ansi+'\n'+parser+'\n'+main)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
PY
