"""Actual update-config reader resolves independent API thread cwd and defaults."""
from pathlib import Path
import tempfile,subprocess
R=Path(__file__).resolve().parents[1];s=(R/'src/api.c').read_text(encoding='utf-8')
a=s.index('static int upd_read_conf(');b=s.index('\nstatic void job_update_check',a)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define UPDATE_CONF "./update.txt"
#define PSP_O_RDONLY 1
static int missing;static const char*text="http://custom.example/version.txt\n";static char opened[768];
static char*getcwd(char*b,size_t n){snprintf(b,n,"ms0:/PSP/GAME/CAT Music/MELODY");return b;}
static int sceIoOpen(const char*p,int f,int m){(void)f;(void)m;snprintf(opened,sizeof(opened),"%s",p);return missing?-1:1;}
static int sceIoRead(int fd,void*b,int n){(void)fd;int len=(int)strlen(text);assert(len<n);memcpy(b,text,len);return len;}
static void sceIoClose(int fd){(void)fd;}
static void upd_trim(char*s){size_t n=strlen(s);while(n&&(s[n-1]=='\r'||s[n-1]==' '))s[--n]=0;}
'''+s[a:b]+r'''
int main(void){char out[1000];assert(!upd_read_conf(out,sizeof(out)));assert(!strcmp(opened,"ms0:/PSP/GAME/CAT Music/MELODY/update.txt"));assert(!strcmp(out,"http://custom.example/version.txt"));missing=1;assert(!upd_read_conf(out,sizeof(out)));assert(!strcmp(out,"http://melody-1320953126.cos.ap-nanjing.myqcloud.com/melody/version.txt"));puts("PASS actual API reader: relocated root independent of thread cwd; custom address retained; missing file official default");}
'''
with tempfile.TemporaryDirectory()as t:
 p=Path(t);(p/'test.c').write_text(code);subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',p/'test.c','-o',p/'test.exe'],check=True);subprocess.run([p/'test.exe'],check=True)
