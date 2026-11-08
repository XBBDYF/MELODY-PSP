"""Exercise the actual production Range adapter with deterministic HTTP responses."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/mv-range-test';O.mkdir(exist_ok=True)
s=(R/'src/mv.c').read_text(encoding='utf-8');start=s.index('static int src_range_read(');end=s.index('\nstatic int src_read_at(',start);body=s[start:end]
prefix=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef unsigned SceUInt;
typedef struct{int status;long long range_start,range_end,range_total,content_length;}Http;
static int g_range_lock=1,g_range_disabled,g_src_fixture,g_range_len,g_range_busy,g_src_segmented;
#define mv_stream_available() (0LL)
static volatile int g_src_cancel,g_src_interrupted;
static char g_src_url[1000]="http://fixture/video.mp4";
static long long g_range_start,nextoff,endoff;
static Http g_range_http;
static unsigned char g_range_buf[64*1024],g_range_previous[64*1024];
static long long g_range_previous_start;
static int g_range_previous_len;
static int locked,opens,closes,badstatus,cancel_on_lock,interrupt_on_read,reads;
static unsigned clockus;
static int sceKernelWaitSema(int s,int n,SceUInt*t){(void)s;(void)n;(void)t;assert(!locked);locked=1;if(cancel_on_lock)g_src_cancel=1;return 0;}
static int sceKernelSignalSema(int s,int n){(void)s;(void)n;assert(locked);locked=0;return 0;}
static unsigned sceKernelGetSystemTimeLow(void){return clockus+=1000;}
static int http_open_range(Http*h,const char*u,long long off,long long end,int tm,char*e,int z){(void)u;(void)tm;(void)e;(void)z;opens++;h->status=badstatus?200:206;h->range_start=off;h->range_end=end;h->range_total=1000000;h->content_length=end-off+1;nextoff=off;endoff=end;return 0;}
static void http_close(Http*h){(void)h;closes++;}
static int http_set_timeout(Http*h,int t){(void)h;(void)t;return 0;}
static int http_read(Http*h,char*b,int len,int idle,volatile int*c,int gen,char*e,int z){(void)h;(void)idle;(void)c;(void)gen;(void)e;(void)z;reads++;if(interrupt_on_read){g_src_interrupted=1;return -1;}if(nextoff>endoff)return 0;if(len>4096)len=4096;if(len>endoff-nextoff+1)len=(int)(endoff-nextoff+1);for(int i=0;i<len;i++)b[i]=(char)((nextoff+i)%251);nextoff+=len;return len;}
#define mlog(...) ((void)0)
'''
tail=r'''
static void reset(void){assert(!locked);g_range_disabled=g_src_cancel=g_src_interrupted=g_range_len=badstatus=cancel_on_lock=interrupt_on_read=0;opens=closes=reads=0;g_range_previous_len=0;}
int main(void){unsigned char b[1000];reset();assert(src_range_read(900000,b,1000,1000000)==1000);for(int i=0;i<1000;i++)assert(b[i]==(unsigned char)((900000+i)%251));assert(opens==1&&closes==1&&!locked);assert(src_range_read(900500,b,500,1000000)==500&&opens==1);for(int i=0;i<500;i++)assert(b[i]==(unsigned char)((900500+i)%251));assert(src_range_read(400000,b,500,1000000)==500&&opens==2);assert(src_range_read(900500,b,500,1000000)==500&&opens==2);for(int i=0;i<500;i++)assert(b[i]==(unsigned char)((900500+i)%251));reset();badstatus=1;assert(src_range_read(900000,b,1000,1000000)==0&&g_range_disabled&&closes==1&&!locked);reset();cancel_on_lock=1;assert(src_range_read(900000,b,1000,1000000)==-1&&!locked&&opens==0);reset();interrupt_on_read=1;assert(src_range_read(900000,b,1000,1000000)==-1&&!locked&&closes==1);reset();assert(src_range_read(999999,b,1000,1000000)==1&&b[0]==(unsigned char)(999999%251));puts("PASS actual production Range bytes/cache/206 admission/end bound/cancel-lock/seek interruption");return 0;}
'''
(O/'test.c').write_text(prefix+body+tail,encoding='utf-8');exe=O/'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True)
subprocess.run([exe],check=True)
