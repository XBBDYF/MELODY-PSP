"""Production cache capacity regression using a filesystem shim."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = '\n'.join(x for x in (ROOT/'src/cache.c').read_text(encoding='utf-8').splitlines() if not x.startswith('#include'))
header = (ROOT/'src/cache.h').read_text(encoding='utf-8').replace('#include "app.h"','')
shim = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <sys/stat.h>
#include <direct.h>
#define APP_DIR ""
typedef struct {long long id;char name[72],artist[72],album[44],art[208];int dur_ms;} Song;
typedef struct {long long st_size;} SceIoStat;
typedef struct {char d_name[256];} SceIoDirent;
static int sceIoGetstat(const char*p,SceIoStat*s){struct _stat64 t;if(_stat64(p,&t))return -1;s->st_size=t.st_size;return 0;}
static int sceIoMkdir(const char*p,int m){(void)m;return _mkdir(p);}
static int sceIoRemove(const char*p){return remove(p);}
static int sceIoDopen(const char*p){(void)p;return -1;}
static int sceIoDread(int f,SceIoDirent*d){(void)f;(void)d;return 0;}
static int sceIoDclose(int f){(void)f;return 0;}
static int sceKernelCreateSema(const char*n,int a,int b,int c,void*d){(void)n;(void)a;(void)b;(void)c;(void)d;return 1;}
static void sceKernelWaitSema(int a,int b,void*c){(void)a;(void)b;(void)c;}
static void sceKernelSignalSema(int a,int b){(void)a;(void)b;}
static void io_lock(void){}
static void io_unlock(void){}
static int fail_commit;
static int host_rename(const char*a,const char*b){if(fail_commit && strstr(a,".tmp")){fail_commit=0;return -1;}return rename(a,b);}
#define rename host_rename
#define mlog(...) ((void)0)
'''
driver = r'''
static void touch(int id,const char*ext){char p[80];snprintf(p,sizeof(p),"cache/%d.%s",id,ext);FILE*f=fopen(p,"wb");assert(f);fputc('x',f);fclose(f);}
int main(int argc,char**argv){
 int count=atoi(argv[1]);(void)argc;
 _mkdir("cache");FILE*f=fopen("cache/index.txt","w");assert(f);
 for(int i=1;i<=count;i++){touch(i,"mp3");touch(i,"ok");fprintf(f,"%d|123|1|fixture%d|artist|album|art\n",i,i);}fclose(f);
 cache_init();printf("physical=%d indexed=%d entry_bytes=%u\n",count,cache_count(),(unsigned)sizeof(CacheEntry));
 if(cache_count()!=count)return 1;
 for(int i=0;i<count;i++){const CacheEntry*e=cache_get(i);assert(e && e->id==i+1);if(i%97==0)assert(cache_complete(e->id));assert(cache_file_bytes(e->id)==1);}
 touch(900000,"mp3");touch(800000,"mp3");touch(800000,"part");
 Song s={0};s.id=900000;strcpy(s.name,"new fixture");cache_add(&s,1);
 assert(cache_file_bytes(1)==1);assert(cache_file_bytes(800000)==1);
 cache_init();assert(cache_count()==count+1);assert(cache_find(900000));
#ifdef CACHE_DISK_INDEX
 int generation=cache_gen();
 fail_commit=1;s.id=1;strcpy(s.name,"failed update");cache_add(&s,1);
 assert(cache_gen()==generation);assert(strcmp(cache_find(1)->name,"failed update"));
 cache_init();assert(cache_count()==count+1);assert(cache_find(900000));
#endif
 s.id=1;strcpy(s.name,"updated fixture");cache_add(&s,1);
 cache_init();assert(cache_count()==count+1);assert(!strcmp(cache_find(1)->name,"updated fixture"));
 cache_remove(2);cache_init();assert(cache_count()==count);assert(!cache_find(2));assert(cache_file_bytes(2)<0);assert(cache_file_bytes(800000)==1);
#ifdef CACHE_DISK_INDEX
 assert(sizeof(g_lookup)+sizeof(g_entries)<=11000);
 assert(cache_snapshot_ids("queue.ids")==count);
 FILE*q=fopen("queue.ids","rb");assert(q);long long tail;
 assert(!fseek(q,(count-1)*8L,SEEK_SET));assert(fread(&tail,8,1,q)==1);assert(tail==900000);fclose(q);
 fail_commit=1;assert(cache_snapshot_ids("queue.ids")==-1);
 q=fopen("queue.ids","rb");assert(q);assert(!fseek(q,(count-1)*8L,SEEK_SET));assert(fread(&tail,8,1,q)==1);assert(tail==900000);fclose(q);
 assert(!cache_has(700000));assert(!cache_has(700000));
 touch(700000,"mp3");cache_adopt(700000,"adopt fixture","artist",100,1);assert(cache_has(700000));
 cache_remove(700000);assert(!cache_has(700000));
 CacheEntry page[3];assert(cache_snapshot_page(page,3,count-2)==2);assert(page[1].id==900000);
 printf("resident_index_and_ring=%u baseline=26624\n",(unsigned)(sizeof(g_lookup)+sizeof(g_entries)));
#endif
 puts("PASS production capacity lookup update restart removal and preservation");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='melody-cache-capacity-') as temp:
    work=Path(temp)
    c,exe=work/'fixture.c',work/'fixture.exe'
    c.write_text(shim+header+source+driver,encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O0','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-format-truncation',str(c),'-o',str(exe)],check=True)
    for count in (70,130,1500,6000):
        directory=work/str(count)
        directory.mkdir()
        subprocess.run([str(exe),str(count)],cwd=directory,check=True)
