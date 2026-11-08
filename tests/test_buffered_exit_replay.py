"""Fresh-process replay of interrupted production cache/download disk state.

Runs actual job_begin/flush/checkpoint/close/cache_init helpers against host files.
Platform I/O is adapted; no decoder, CDN or PSP device is simulated as passing.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DL = (ROOT / 'src/dl.c').read_text(encoding='utf-8')
CACHE = (ROOT / 'src/cache.c').read_text(encoding='utf-8')


def function(source, signature):
    start = source.index(signature)
    end = source.index('\n}', start) + 2
    return source[start:end] + '\n'


HEADER = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
typedef long long SceOff;
typedef struct { long long st_size; } SceIoStat;
typedef struct { int sock; } Http;
typedef struct { long long id; int dur_ms; char name[72],artist[72],album[44],art[208]; } Song;
typedef struct { long long id; int dur_ms,bytes; char name[72],artist[72],album[44],art[208]; } CacheEntry;
#define CACHE_MAX 64
#define CACHE_DISK_INDEX 1
#define APP_DIR ""
#define PSP_O_RDONLY _O_RDONLY
#define PSP_O_WRONLY _O_WRONLY
#define PSP_O_RDWR _O_RDWR
#define PSP_O_CREAT _O_CREAT
#define PSP_O_TRUNC _O_TRUNC
#define PSP_SEEK_SET SEEK_SET
#define PSP_SEEK_END SEEK_END
#define mlog(...) ((void)0)
#include "dl_progress_contract.h"
#define io_lock() ((void)0)
#define io_unlock() ((void)0)
static int sceIoOpen(const char*p,int f,int m){(void)m;return _open(p,f|_O_BINARY,_S_IREAD|_S_IWRITE);}
static int sceIoClose(int f){return _close(f);}
static int sceIoRead(int f,void*b,int n){return _read(f,b,n);}
static int sceIoWrite(int f,const void*b,int n){return _write(f,b,n);}
static long long sceIoLseek(int f,long long o,int w){return _lseeki64(f,o,w);}
static int sceIoGetstat(const char*p,SceIoStat*s){struct _stati64 st;if(_stati64(p,&st))return -1;s->st_size=st.st_size;return 0;}
static int sceIoRemove(const char*p){return remove(p);}
static int sceIoMkdir(const char*p,int m){(void)m;return _mkdir(p);}
static int sceIoSync(const char*p,int m){(void)p;(void)m;return 0;}
static int sceKernelCreateSema(const char*n,int a,int b,int c,void*d){(void)n;(void)a;(void)b;(void)c;(void)d;return 1;}
static void sceKernelWaitSema(int a,int b,void*c){(void)a;(void)b;(void)c;}
static void sceKernelSignalSema(int a,int b){(void)a;(void)b;}
static void sceKernelDelayThread(int n){(void)n;}
static unsigned sceKernelGetSystemTimeLow(void){return 3000000;}
static void http_close(Http*h){h->sock=-1;}
static int audio_file_ok(const char*p){(void)p;return 1;}
'''
cache_code = CACHE[CACHE.index('#define INDEX_PATH'):CACHE.index('void cache_prune(void)')]
layout = DL[DL.index('#define DL_MAX_PARTS'):DL.index('static DlJob g_stream;')]
helpers = r'''
static int g_parts=4,g_qgen,g_queue_dirty;
static unsigned char g_wbuf[DL_MAX_PARTS][DL_WBUF];
static void checkpoint_save(DlJob*j);
static int checkpoint_load(DlJob*j);
static long long job_prefix(DlJob*j);
static long long advance_chunks(DlJob*j,long long from,int n);
'''
for signature in ['static int chunk_len(', 'static long long chunk_off(',
                  'static void job_reset(', 'static void job_close_net(',
                  'static void job_close_file(', 'static int part_marker(',
                  'static int flush_part(', 'static int job_begin(',
                  'static void job_finish(DlJob *j)\n{',
                  'static long long job_prefix(DlJob *j)\n{',
                  'static unsigned int hash_bytes(', 'static int hash_disk(',
                  'static int checkpoint_load(DlJob *j)\n{',
                  'static void checkpoint_save(DlJob *j)\n{',
                  'static long long advance_chunks(DlJob *j, long long from, int n)\n{']:
    helpers += function(DL, signature)
TAIL = r'''
int main(int argc,char**argv){
 assert(argc==3);const char*mode=argv[1];const char*scenario=argv[2];
 Song song={0};song.id=42;strcpy(song.name,"fixture");song.dur_ms=180000;
 int total=4*DL_CHUNK_SIZE;int prefix=!strcmp(scenario,"fullprefix")?total:2*DL_CHUNK_SIZE;
 const char*url="https://fixture/song";DlJob job;cache_init();
 if(!strcmp(mode,"first")){
  assert(!cache_has(song.id));assert(job_begin(&job,&song,url,total,1,7)==0);
  for(int off=0;off<prefix;off+=DL_WBUF){job.woff[0]=off;job.wlen[0]=DL_WBUF;memset(g_wbuf[0],0x5a,DL_WBUF);assert(!flush_part(&job,0));}
  if(!strcmp(scenario,"sparse")){job.woff[1]=total-DL_WBUF;job.wlen[1]=DL_WBUF;memset(g_wbuf[1],0x6b,DL_WBUF);assert(!flush_part(&job,1));}
  assert(job_prefix(&job)==prefix);assert(!cache_has(song.id));
  if(!strcmp(scenario,"abrupt")){sceIoClose(job.fd);job.fd=-1;}
  else {job_close_net(&job);job_close_file(&job);}
  if(!strcmp(scenario,"torn")){FILE*f=fopen("cache/42.part","w");assert(f);fputs("MDL1 42 torn",f);fclose(f);}
  assert(!cache_has(song.id));assert(!has_ok(song.id));return 0;
 }
 /* This invocation has fresh C globals; cache_has is job_play's routing test. */
 assert(!cache_has(song.id));
 if(!strcmp(scenario,"changedurl"))url="https://fixture/song?new=1";
 assert(job_begin(&job,&song,url,total,1,2)==0);assert(job.active&&!job.error&&!job.complete);
 int restart=!strcmp(scenario,"torn")||!strcmp(scenario,"changedurl")||!strcmp(scenario,"abrupt");
 assert(job_prefix(&job)==(restart?0:prefix));assert(!has_ok(song.id));
 /* A full checkpoint can finish; a partial checkpoint extends at true prefix. */
 int off=(int)job_prefix(&job);
 for(;off<total;off+=DL_WBUF){job.woff[0]=off;job.wlen[0]=DL_WBUF;memset(g_wbuf[0],0x5a,DL_WBUF);assert(!flush_part(&job,0));}
 assert(job_prefix(&job)==total);job_finish(&job);assert(job.complete&&!job.error);assert(cache_has(song.id)&&has_ok(song.id));
 puts("PASS fresh-process replay and verified completion");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='buffered-exit-replay-') as temp:
    root = Path(temp)
    c = root / 'fixture.c'
    exe = root / 'fixture.exe'
    c.write_text(HEADER + cache_code + layout + helpers + TAIL, encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c11', '-O0', '-Wall',
                    '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
                    '-I', str(ROOT / 'src'), str(c), '-o', str(exe)], check=True)
    for case in ['prefix', 'sparse', 'torn', 'changedurl', 'fullprefix', 'abrupt']:
        directory = root / case
        directory.mkdir()
        subprocess.run([str(exe), 'first', case], cwd=directory, check=True)
        subprocess.run([str(exe), 'replay', case], cwd=directory, check=True)
        print(case, 'PASS')
    print('PASS: interrupted disk cache does not block same-song fresh-process replay')
    print('LIMIT: platform I/O adapter; actual PSP decode and live CDN not exercised')



