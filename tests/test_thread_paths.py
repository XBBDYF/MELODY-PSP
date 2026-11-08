"""Exercise the real shared binder with separate PSP-thread I/O cwd states."""
from pathlib import Path
import re,subprocess,tempfile
R=Path(__file__).resolve().parents[1]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define THREAD_PATHS_HOST_TEST
static char thread_cwd[8][512];static int thread;static const char*app_root="ms0:/PSP/GAME/CAT Music/MELODY";
static char*getcwd(char*out,size_t n){snprintf(out,n,"%s",app_root);return out;}
static int sceIoChdir(const char*path){snprintf(thread_cwd[thread],512,"%s",path);return 0;}
#include "thread_paths.h"
static int open_cached_song(void){return strcmp(thread_cwd[thread],app_root)?-1:1;}
int main(void){for(thread=0;thread<8;thread++){strcpy(thread_cwd[thread],"ms0:/");assert(open_cached_song()<0);assert(!melody_bind_thread_directory());assert(open_cached_song()>0);}thread=1;strcpy(thread_cwd[thread],"ms0:/");assert(!melody_bind_thread_directory()&&open_cached_song()>0);app_root="ef0:/PSP/GAME/CAT Audio/MELODY";thread=4;assert(!melody_bind_thread_directory()&&open_cached_song()>0);puts("PASS real shared binding: independent decode/download/MV/export worker cwd, thread restart and relocated ms0/ef0");}
'''
with tempfile.TemporaryDirectory()as t:
 p=Path(t);(p/'test.c').write_text(code);subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-I'+str(R/'src'),p/'test.c','-o',p/'test.exe'],check=True);subprocess.run([p/'test.exe'],check=True)
for name,functions in {'audio.c':['decode_thread','audio_thread'],'dl.c':['dl_thread'],'mv.c':['src_thread','src_worker','mv_thread'],'mvexport.c':['mvx_worker'],'mvsrc.c':['mvsrc_thread']}.items():
 s=(R/'src'/name).read_text(encoding='utf-8');assert '#include "thread_paths.h"'in s
 for fn in functions:
  m=re.search(r'static int '+fn+r'\(SceSize[^;{}]*\)\s*\{',s);assert m,fn
  assert 'melody_bind_thread_directory();'in s[m.end():m.end()+100],fn
print('PASS every production audio/download/MV/export entry binds before I/O')
