"""Run production home-card load/save using local file shims, no network."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/home-cards-cache-test';O.mkdir(exist_ok=True)
s=(R/'src/api.c').read_text(encoding='utf-8');a=s.index('#define HOME_CARDS_CACHE');b=s.index('static void cards_fetch(',a)
prefix=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "app.h"
typedef int SceUID;
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 512
#define PSP_O_TRUNC 1024
static CardList g_cards;static FILE*fds[8];
static const char*local(const char*p){const char*q=strrchr(p,'/');return q?q+1:p;}
static int sceIoOpen(const char*p,int flags,int mode){(void)mode;FILE*f=fopen(local(p),flags&2?"wb":"rb");if(!f)return -1;for(int i=1;i<8;i++)if(!fds[i]){fds[i]=f;return i;}fclose(f);return -1;}
static int sceIoRead(int f,void*p,unsigned n){return (int)fread(p,1,n,fds[f]);}
static int sceIoWrite(int f,const void*p,unsigned n){return (int)fwrite(p,1,n,fds[f]);}
static int sceIoClose(int f){int r=fclose(fds[f]);fds[f]=0;return r;}
static int sceIoRemove(const char*p){return remove(local(p));}
static int sceIoRename(const char*a,const char*b){return rename(local(a),local(b));}
#define mlog(...) ((void)0)
'''
tail=r'''
int main(void){remove("home-cards.bin");g_cards.count=2;g_cards.items[0].id=123;g_cards.items[1].id=456;strcpy(g_cards.title,"Home cache");strcpy(g_cards.items[0].cover,"http://fixture/cover.jpg");home_cards_save();memset(&g_cards,0,sizeof(g_cards));home_cards_load();assert(g_cards.count==2&&g_cards.items[0].id==123&&!strcmp(g_cards.items[0].cover,"http://fixture/cover.jpg"));FILE*f=fopen("home-cards.bin","wb");fputs("bad",f);fclose(f);home_cards_loaded=0;g_cards.count=0;home_cards_load();assert(g_cards.count==0);puts("PASS production home cards restart/offline metadata cache; invalid/truncated cache ignored");return 0;}
'''
(O/'test.c').write_text(prefix+s[a:b]+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(R/'src'),O/'test.c','-o',exe],check=True);subprocess.run([exe],cwd=O,check=True)
