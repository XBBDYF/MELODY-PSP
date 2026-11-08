"""Host regressions execute production download scheduling and disk publication."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/dl.c').read_text(encoding='utf-8')
# Keep production job layout and implementation, replacing only platform boundaries.
source = source[source.index('#define DL_MAX_PARTS'):source.index('/* 线程')]
source = source[:source.index('static int job_begin(')] + source[source.index(' /* 从 0 开始'):]
source = source[:source.index('/* .part v1')] + source[source.index('/* The low watermark'):]
source = source.replace('static void checkpoint_save(DlJob *j);', 'static void checkpoint_save(DlJob *j) {(void)j;}')
source = source.replace('static void job_finish(DlJob *j);', 'static void job_finish(DlJob *j) { j->active = 0; j->complete = !j->error; }')
api = (ROOT / 'src/dl.c').read_text(encoding='utf-8')
api = api[api.index('long long dl_stream_available('):api.index('\nint dl_stream_total(')]
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dl_progress_contract.h"
typedef long long SceOff;
typedef struct { int sock,status; long long range_start; } Http;
typedef struct { long long id; char name[80]; } Song;
typedef struct { int unused; } DlItem;
#define APP_DIR "fixture/"
#define PSP_O_WRONLY 1
#define PSP_O_CREAT 2
#define PSP_O_TRUNC 4
#define PSP_SEEK_SET 0
static unsigned clock_us,open_cost_us;
static int opens,read_min_open,read_calls,reads_at_first_open,writes,writes_at_first_open;
static unsigned sceKernelGetSystemTimeLow(void){return clock_us;}
static void sceKernelDelayThread(int n){(void)n;}
static void sceKernelWaitSema(int a,int b,void*c){(void)a;(void)b;(void)c;}
static void sceKernelSignalSema(int a,int b){(void)a;(void)b;}
static void mlog(const char*f,...){(void)f;}
static int net_reconnect_poll(void){return 0;}
static void http_close(Http*h){h->sock=-1;}
static int http_open_range_cancel(Http*h,const char*u,long long o,long long e,int t,volatile int*c,int g,char*b,int n){(void)c;(void)g;(void)u;(void)e;(void)t;(void)b;(void)n;if(!opens){reads_at_first_open=read_calls;writes_at_first_open=writes;}clock_us+=open_cost_us;h->sock=++opens;h->status=206;h->range_start=o;return 0;}
static long long disk_pos;
static unsigned char disk[4*1024*1024];
static int write_limit, write_fail_after=-1, read_budget;
static int sceIoOpen(const char*p,int f,int m){(void)p;(void)f;(void)m;return 1;}
static int sceIoClose(int f){(void)f;return 0;}
static int sceIoRemove(const char*p){(void)p;return 0;}
static long long sceIoLseek(int f,SceOff o,int w){(void)f;(void)w;disk_pos=o;return o;}
static int sceIoWrite(int f,const void*b,int n){(void)f;if(write_fail_after==0)return -1;if(write_limit&&n>write_limit)n=write_limit;memcpy(disk+disk_pos,b,n);writes++;disk_pos+=n;if(write_fail_after>0)write_fail_after--;return n;}
static int http_read(Http*h,char*b,int n,int t,void*c,int g,char*e,int z){(void)h;(void)t;(void)c;(void)g;(void)e;(void)z;read_calls++;if(h->sock<read_min_open||read_budget<=0)return -1;if(n>read_budget)n=read_budget;memset(b,0x5a,n);read_budget-=n;return n;}
'''
tail = r'''
static void fixture(void){job_reset(&g_stream);g_stream.active=1;g_stream.is_stream=1;g_stream.gen=7;g_stream.total=30*DL_CHUNK_SIZE;g_stream.chunk_size=DL_CHUNK_SIZE;g_stream.nchunks=30;g_stream.nparts=6;g_stream.fd=1;clock_us=100;write_limit=0;write_fail_after=-1;read_budget=0;}
int main(int argc,char**argv){assert(argc==2);fixture();
if(!strcmp(argv[1],"retained")){g_stream.cgot[0]=16384;assert(dl_stream_available(7)==16384);}
else if(!strcmp(argv[1],"reserve")){assign_parts(&g_stream);assert(g_stream.pend[0]<=DL_CHUNK_SIZE);}
else if(!strcmp(argv[1],"flush")){assign_parts(&g_stream);read_budget=4096;pump_part(&g_stream,0);assert(dl_stream_available(7)==4096);assert(disk[4095]==0x5a);}
else if(!strcmp(argv[1],"readable")){advance_chunks(&g_stream,0,4096);assert(dl_stream_readable(7,2048,8192)==2048);assert(dl_stream_readable(7,4096,8192)==0);assert(dl_stream_readable(7,0,0)==0);assert(dl_stream_readable(7,-1,10)==0);assert(dl_stream_readable(6,1000000,4096)==0);job_pump(&g_stream);assert(g_stream.read_off==4096);g_cmd_pending=1;g_cmd_gen=8;assert(dl_stream_readable(7,900000,1)==0);g_cmd_pending=0;assert(g_read_off==4096);g_read_gen=6;g_read_off=999999;job_pump(&g_stream);assert(g_stream.read_off==4096);}
else if(!strcmp(argv[1],"resumebuffer")){advance_chunks(&g_stream,0,4*DL_CHUNK_SIZE);g_stream.prun[0]=4;g_stream.prun_end[0]=5;g_stream.ppos[0]=4*DL_CHUNK_SIZE;g_stream.pend[0]=5*DL_CHUNK_SIZE;g_stream.http[0].sock=1;g_stream.cstate[4]=1;g_stream.prun[1]=5;g_stream.prun_end[1]=6;g_stream.ppos[1]=5*DL_CHUNK_SIZE+4096;g_stream.pend[1]=6*DL_CHUNK_SIZE;g_stream.woff[1]=5*DL_CHUNK_SIZE;g_stream.wlen[1]=4096;g_stream.cstate[5]=1;memset(g_wbuf[1],0x5a,4096);assign_parts(&g_stream);assert(g_stream.cgot[5]==4096);assert(disk[5*DL_CHUNK_SIZE+4095]==0x5a);}
else if(!strcmp(argv[1],"shortwrite")){assign_parts(&g_stream);write_limit=37;read_budget=4096;pump_part(&g_stream,0);assert(dl_stream_available(7)==4096);assert(g_stream.done_bytes==4096);for(int i=0;i<4096;i++)assert(disk[i]==0x5a);}
else if(!strcmp(argv[1],"unknown")){g_stream.total=0;g_stream.chunk_size=0;g_stream.nchunks=1;g_stream.nparts=1;assign_parts(&g_stream);read_budget=4096;job_pump(&g_stream);assert(dl_stream_available(7)==4096);assert(dl_stream_readable(7,0,8192)==4096);}
else if(!strcmp(argv[1],"falsecomplete")){g_stream.total=2*DL_CHUNK_SIZE;g_stream.nchunks=2;g_stream.done_bytes=g_stream.total;advance_chunks(&g_stream,DL_CHUNK_SIZE,DL_CHUNK_SIZE);job_pump(&g_stream);assert(!g_stream.complete);assert(dl_stream_available(7)==0);}
else if(!strcmp(argv[1],"retry")){assign_parts(&g_stream);g_stream.watch_prefix=0;g_stream.prefix_t=1;clock_us=2000002;g_stream.done_bytes=1000000;read_min_open=2;read_budget=4096;job_pump(&g_stream);assert(dl_stream_available(7)==4096);assert(g_stream.prefix_t==clock_us);}
else if(!strcmp(argv[1],"hole")){advance_chunks(&g_stream,0,4*DL_CHUNK_SIZE);g_stream.cgot[4]=8192;g_stream.cstate[4]=1;g_stream.prun[0]=4;g_stream.prun_end[0]=8;g_stream.ppos[0]=4*DL_CHUNK_SIZE+8192;g_stream.pend[0]=8*DL_CHUNK_SIZE;g_stream.http[0].sock=1;g_stream.prun[1]=8;g_stream.prun_end[1]=12;g_stream.ppos[1]=8*DL_CHUNK_SIZE;g_stream.pend[1]=12*DL_CHUNK_SIZE;g_stream.http[1].sock=1;g_stream.woff[0]=g_stream.ppos[0];g_stream.woff[1]=g_stream.ppos[1];for(int i=4;i<12;i++)g_stream.cstate[i]=1;assert(dl_stream_readable(7,4*DL_CHUNK_SIZE,16384)==8192);read_budget=4096;job_pump(&g_stream);assert(dl_stream_available(7)==4*DL_CHUNK_SIZE+12288);}
else if(!strcmp(argv[1],"assignbudget")){advance_chunks(&g_stream,0,4*DL_CHUNK_SIZE);open_cost_us=500000;assign_parts(&g_stream);assert(opens==1);assert(clock_us==500100);}
else if(!strcmp(argv[1],"pumpbeforeopen")){advance_chunks(&g_stream,0,4*DL_CHUNK_SIZE);g_stream.prun[0]=4;g_stream.prun_end[0]=5;g_stream.ppos[0]=4*DL_CHUNK_SIZE;g_stream.pend[0]=5*DL_CHUNK_SIZE;g_stream.woff[0]=g_stream.ppos[0];g_stream.cstate[4]=1;g_stream.http[0].sock=100;open_cost_us=500000;read_budget=4096;job_pump(&g_stream);assert(reads_at_first_open>0);assert(writes_at_first_open>0);assert(dl_stream_available(7)==4*DL_CHUNK_SIZE+4096);assert(opens==1);}
else if(!strcmp(argv[1],"lowowner")){assign_parts(&g_stream);int before=opens;read_budget=4096;job_pump(&g_stream);assert(opens==before);assert(dl_stream_available(7)==4096);}
puts("PASS production dl prefix regression");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='dl-prefix-') as tmp:
    c = Path(tmp)/'test.c'; exe = Path(tmp)/'test.exe'
    c.write_text(prefix+source+api+tail, encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O0','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-I',str(ROOT/'src'),str(c),'-o',str(exe)],check=True)
    failures=[]
    for case in ['retained','reserve','flush','hole','readable','shortwrite','unknown','falsecomplete','retry','resumebuffer','assignbudget','pumpbeforeopen','lowowner']:
        result=subprocess.run([str(exe),case],capture_output=True,text=True)
        print(case, 'PASS' if result.returncode==0 else 'FAIL', result.stdout, result.stderr)
        if result.returncode: failures.append(case)
    assert not failures, failures
if __name__ == '__main__':
    pass
