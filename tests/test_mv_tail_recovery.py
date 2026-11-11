"""Run production MV source/resume functions against deterministic HTTP and RAM files."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.tmp/mv-tail-recovery'
OUT.mkdir(parents=True, exist_ok=True)
s = (ROOT / 'src/mv.c').read_text(encoding='utf-8')
a = s.index('static void src_download_finished(')
b = s.index('\n/* Never exit/delete', a)
production = s[a:b]
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
typedef int SceSize;
typedef int SceUID;
typedef long long SceOff;
typedef struct {long long st_size;} SceIoStat;
#define PSP_O_WRONLY 1
#define PSP_O_CREAT 2
#define PSP_O_TRUNC 4
#define PSP_O_RDWR 8
#define PSP_SEEK_SET 0
#define APP_DIR ""
static void mlog(const char*format,...){(void)format;}
static const char g_src_url[]="http://fixture/mv",g_src_part[]="fixture.part",g_src_final[]="fixture.mp4";
static volatile int g_src_cancel,g_src_err,g_src_done,g_src_renaming,g_mv_paused,g_src_rate_bps;
static long long g_src_total,g_src_avail;
static unsigned char g_src_buf[16384],disk[65536],expected[65536];
static int mode,total,missing,ranges,renames,completed_marks,metadata_marks,exists,position,size;
static long long cursor,limit;
static unsigned clock_us;
static unsigned sceKernelGetSystemTimeLow(void){return clock_us+=1000;}
static unsigned sceKernelCheckThreadStack(void){return 65536;}
static void sceKernelDelayThread(unsigned us){clock_us+=us;}
static int mv_stream_idle_expired(unsigned now,unsigned last){return (unsigned)(now-last)>=20000000u;}
static void src_publish_total(long long n){g_src_total=n;}
static void src_publish_available(long long n){g_src_avail=n;}
static void src_mark_done(void){metadata_marks++;if(g_src_done)completed_marks++;}
static int sceIoGetstat(const char*p,SceIoStat*st){(void)p;if(!exists)return -1;st->st_size=size;return 0;}
static int sceIoOpen(const char*p,int flags,int permissions){(void)permissions;if(strcmp(p,g_src_part))return 2;if(flags&PSP_O_TRUNC)size=0;exists=1;position=0;return 1;}
static SceOff sceIoLseek(int fd,SceOff offset,int whence){assert(fd==1&&whence==PSP_SEEK_SET);position=(int)offset;return offset;}
static int sceIoRead(int fd,void*b,int n){assert(fd==1);if(n>size-position)n=size-position;memcpy(b,disk+position,(size_t)n);position+=n;return n;}
static int sceIoWrite(int fd,const void*b,int n){if(fd!=1)return n;if(mode==10)return -1;assert(position+n<=(int)sizeof(disk));memcpy(disk+position,b,(size_t)n);position+=n;if(position>size)size=position;return n;}
static int sceIoClose(int fd){return fd==1&&mode==11?-1:0;}
static int sceIoSync(const char*p,int flags){(void)p;(void)flags;return 0;}
static int sceIoRemove(const char*p){assert(!strcmp(p,g_src_final));return 0;}
static int sceIoRename(const char*from,const char*to){assert(!strcmp(from,g_src_part)&&!strcmp(to,g_src_final));renames++;return 0;}
static void http_close_stub(Http*h){h->sock=-1;}
#define http_close http_close_stub
static int http_open_bulk_stub(Http*h,const char*u,int t,char*e,int cap){(void)u;(void)t;(void)e;(void)cap;h->sock=1;h->status=200;h->content_length=total;cursor=0;limit=total-missing;return 0;}
#define http_open_bulk http_open_bulk_stub
static int http_open_range_stub(Http*h,const char*u,long long start,long long end,int t,char*e,int cap){
 (void)u;(void)t;(void)e;(void)cap;ranges++;h->sock=1;h->status=206;h->range_start=start;h->range_end=end;h->range_total=total;h->content_length=end-start+1;cursor=start;limit=end+1;
 if(mode==2)h->range_start++;
 if(mode==3)h->range_end--;
 if(mode==4)h->range_total++;
 if(mode==5)h->content_length--;
 if(mode==6)h->status=200;
 if(mode==7)limit=start+1;
 if(mode==8)return -1;
 return 0;
}
#define http_open_range http_open_range_stub
static int http_read_stub(Http*h,char*b,int n,int idle,volatile int*cancel,int gen,char*e,int cap){
 (void)h;(void)idle;(void)cancel;(void)gen;(void)e;(void)cap;
 if(cursor<limit){if(n>limit-cursor)n=(int)(limit-cursor);memcpy(b,expected+cursor,(size_t)n);cursor+=n;return n;}
 if(mode==9){g_src_cancel=1;return -3;}
 if(mode==1||mode==7)return 0;
 clock_us+=1000000u;return -1;
}
#define http_read http_read_stub
'''
tail = r'''
int main(int argc,char**argv){
 assert(argc==4);mode=atoi(argv[1]);missing=atoi(argv[2]);int initial=atoi(argv[3]);total=32768+missing;
 for(int i=0;i<total;i++)expected[i]=(unsigned char)((i*37+i/251)%256);
 if(initial){exists=1;size=initial==2?total:10000;memcpy(disk,expected,(size_t)size);if(initial==3)disk[0]^=1;}
 int original_size=size;unsigned char original[65536];memcpy(original,disk,(size_t)size);
 src_thread(0,NULL);
 if(mode==0||mode==1){
  if(initial==3){assert(g_src_err&&!g_src_done&&!renames&&size==original_size&&!memcmp(disk,original,(size_t)size));}
  else {assert(!g_src_err&&g_src_done&&renames==1&&completed_marks==1);assert(size==total&&!memcmp(disk,expected,(size_t)total));assert(g_src_avail==total);assert(ranges==(initial==2?2:initial?3:1));}
 }else{
  assert(!g_src_done&&!renames&&!completed_marks);
  if(mode==9)assert(g_src_cancel&&!g_src_err&&ranges==0);
  else assert(g_src_err);
  if(mode>=2&&mode<=8){assert(ranges==2);assert(size==total-missing+(mode==7?2:0));assert(!memcmp(disk,expected,(size_t)size));}
  if(mode==10)assert(size==0);
  if(mode==11)assert(size==total&&!memcmp(disk,expected,(size_t)size));
 }
 printf("PASS tail mode=%d missing=%d oldpart=%d requests=%d size=%d renamed=%d\n",mode,missing,initial,ranges,size,renames);return 0;
}
'''
(OUT / 'test.c').write_text(prefix + production + tail, encoding='utf-8')
exe = OUT / 'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'src'), str(OUT/'test.c'), '-o', str(exe)], check=True)
cases = [(0,5573,0),(0,12017,0),(1,5573,0)]
cases += [(mode,5573,0) for mode in range(2,12)]
cases += [(0,5573,initial) for initial in (1,2,3)]
for mode, missing, initial in cases:
    subprocess.run([str(exe), str(mode), str(missing), str(initial)], check=True)
print(f'PASS {len(cases)} production MV tail recovery cases; byte equality, bounded attempts, retained partials')
