"""Host execution of production raw parser/cache functions; no real IO paths."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import re
ROOT=Path(__file__).resolve().parents[1]
def extract(text,name):
    m=re.search(r'^static (?:int|void) '+name+r'\([^\n]*\)\s*\{',text,re.M)
    start=text.index('{',m.start());depth=0
    for i in range(start,len(text)):
        depth += (text[i]=='{')-(text[i]=='}')
        if not depth:return text[m.start():i+1]
def execute(source):
    with tempfile.TemporaryDirectory() as d:
        c=Path(d)/'test.c';e=Path(d)/'test.exe';c.write_text(source,encoding='utf-8')
        subprocess.run(['gcc','-std=c99',str(c),'-o',str(e)],check=True,capture_output=True)
        subprocess.run([str(e)],check=True,capture_output=True)
class AuthIdentityTests(unittest.TestCase):
    def test_raw_session_validation_before_commit(self):
        api=(ROOT/'src/api.c').read_text(encoding='utf-8')
        execute(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char g_login_cookie[1280]="OLD",g_login_name[48]="old";
static long long g_login_uid=11;static int g_login_vip;
static void safe_copy(char*d,int n,const char*s){snprintf(d,n,"%s",s);}
'''+extract(api,'login_parse')+r'''
int main(void){
char good[]="cookie=SYNTHETIC\nname=fixture\nuid=22\nvip=1\n";
assert(login_parse(good,1)==0 && g_login_uid==22);
char bad[]="cookie=NEW\nuid=wrong\n";
assert(login_parse(bad,1)<0 && g_login_uid==22 && !strcmp(g_login_cookie,"SYNTHETIC"));
char over[1500];memcpy(over,"cookie=",7);memset(over+7,'a',1280);strcpy(over+1287,"\nuid=44\n");
assert(login_parse(over,1)<0 && g_login_uid==22);
char partial[]="cookie=NEW\nuid=44";
assert(login_parse(partial,1)<0 && g_login_uid==22);
return 0;}
''')
    def test_uid_cache_fast_path_never_reuses_other_account(self):
        api=(ROOT/'src/api.c').read_text(encoding='utf-8')
        execute(r'''
#include <assert.h>
#include <stdio.h>
#define APP_DIR "FIXTURE_ONLY/"
static long long g_login_uid=22,g_liked_cache_uid=11,g_liked_pl=111;
static int g_liked_total=7;
static FILE *fixture_open(const char*p,const char*m){(void)p;(void)m;return NULL;}
#define fopen fixture_open
'''+extract(api,'liked_pl_load')+r'''
int main(void){liked_pl_load();assert(g_liked_pl==0 && g_liked_total==0 && g_liked_cache_uid==0);
g_liked_cache_uid=22;g_liked_pl=222;g_liked_total=0;liked_pl_load();assert(g_liked_pl==222);return 0;}
''')
if __name__=='__main__':unittest.main()
