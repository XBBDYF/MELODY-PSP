"""Actual playlist storage functions with host file IO shims: persistence and rollback."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/playlist-favorites-test';O.mkdir(exist_ok=True)
s=(R/'src/store.c').read_text(encoding='utf-8');a=s.index('#define PLAYLIST_FAV_PATH')
prefix=r'''
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "app.h"
typedef int SceUID;
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 512
#define PSP_O_TRUNC 1024
static FILE*handles[16];static int fail_write;
static const char*path(const char*p){const char*q=strrchr(p,'/');return q?q+1:p;}
static int sceIoOpen(const char*p,int flags,int mode){(void)mode;FILE*f=fopen(path(p),flags&PSP_O_WRONLY?"wb":"rb");if(!f)return -1;for(int i=1;i<16;i++)if(!handles[i]){handles[i]=f;return i;}fclose(f);return -1;}
static int sceIoRead(int f,void*p,unsigned n){return (int)fread(p,1,n,handles[f]);}
static int sceIoWrite(int f,const void*p,unsigned n){if(fail_write)return -1;return (int)fwrite(p,1,n,handles[f]);}
static int sceIoClose(int f){int r=fclose(handles[f]);handles[f]=0;return r;}
static int sceIoRemove(const char*p){return remove(path(p));}
static int sceIoRename(const char*a,const char*b){return rename(path(a),path(b));}
'''
tail=r'''
int main(void){remove("playlist-favorites.bin");remove("playlist-favorites.bin.old");remove("playlist-favorites.bin.tmp");Card c={0};c.id=12345678901LL;snprintf(c.name,sizeof(c.name),"Saved playlist");assert(store_playlist_toggle(&c)==0&&store_playlist_has(c.id));playlist_fav_loaded=0;playlist_fav_n=0;assert(store_playlist_has(c.id));Card out[24];assert(store_playlist_snapshot(out,24)==1&&!strcmp(out[0].name,c.name));fail_write=1;Card d=c;d.id=222;assert(store_playlist_toggle(&d)==-1&&!store_playlist_has(d.id)&&store_playlist_has(c.id));fail_write=0;assert(store_playlist_toggle(&c)==1&&!store_playlist_has(c.id));playlist_fav_loaded=0;assert(store_playlist_snapshot(out,24)==0);puts("PASS actual playlist add/remove, restart persistence, metadata and failed-write rollback");return 0;}
'''
(O/'test.c').write_text(prefix+s[a:s.index('\nint store_show_speed(',a)]+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(R/'src'),O/'test.c','-o',exe],check=True);subprocess.run([exe],cwd=O,check=True)
