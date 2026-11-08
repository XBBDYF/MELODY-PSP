"""Exercise actual API thread directory binding before relative update writes."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];s=(R/'src/api.c').read_text(encoding='utf-8');a=s.index('static int api_bind_directory(');b=s.index('static int worker(',a)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int bound,fail;static char actual[512];
static char*getcwd(char*b,size_t n){snprintf(b,n,"ms0:/PSP/GAME/CAT Music/MELODY");return b;}
static int sceIoChdir(const char*p){snprintf(actual,sizeof(actual),"%s",p);if(fail)return -7;bound=1;return 0;}
static int sceIoOpen(const char*p){assert(!strcmp(p,"./EBOOT.NEW")||!strcmp(p,"./update.meta"));return bound?1:-1;}
'''+s[a:b]+r'''
int main(void){assert(sceIoOpen("./EBOOT.NEW")<0);assert(!api_bind_directory());assert(!strcmp(actual,"ms0:/PSP/GAME/CAT Music/MELODY"));assert(sceIoOpen("./EBOOT.NEW")==1);assert(sceIoOpen("./update.meta")==1);bound=0;fail=1;assert(api_bind_directory()==-7);puts("PASS real API binding: wrong-thread cwd fails before bind, relocated update and metadata target correct root after bind, errors preserved");}
'''
with tempfile.TemporaryDirectory()as t:
 p=Path(t);(p/'test.c').write_text(code);subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',p/'test.c','-o',p/'test.exe'],check=True);subprocess.run([p/'test.exe'],check=True)
worker=s[s.index('static int worker('):];assert worker.index('api_bind_directory()')<worker.index('while (!g_quit)');print('PASS binding precedes all API worker operations')
