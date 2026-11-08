"""Compile production worker helpers against a deterministic fake PSP kernel."""
from pathlib import Path
import subprocess

R = Path(__file__).resolve().parents[1]
O = R / '.tmp/mv-stream-lifecycle'
O.mkdir(parents=True, exist_ok=True)
s = (R / 'src/mv.c').read_text(encoding='utf-8')
a = s.index('static int src_worker(')
b = s.index('\nstatic const char *src_path(', a)
a2 = s.index('int mv_stream_active(void)')
b2 = s.index('\nint mv_stream_done(', a2)
prefix = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
typedef int SceUID;
typedef unsigned SceSize;
#define MV_STREAM_PRIO 0x16
#define MV_STREAM_STACK 0x10000
#define PSP_THREAD_ATTR_USER 1
#define mlog(...) ((void)0)
static SceUID g_src_thid=-1,g_src_wake=-1,g_range_lock=-1;
static volatile int g_src_running,g_src_err;
static int creates,starts,deletes,signals,sessions,waits,fail_start,fail_signal,fail_create;
static int cleanup_live,queued_during_retirement;
static jmp_buf finished;
static int src_queue_session(void);
static int sceKernelCreateSema(const char*n,int a,int initial,int max,void*p){
 (void)a;(void)p;assert(max==1);if(!strcmp(n,"mv_range")){assert(initial==1);return 10;}
 assert(!strcmp(n,"mv_wake")&&initial==0);return 11;
}
static int sceKernelCreateThread(const char*n,int(*entry)(SceSize,void*),int prio,int stack,int attr,void*p){
 (void)entry;(void)p;assert(!strcmp(n,"mv_dl")&&prio==0x16&&stack==65536&&attr==1);creates++;return fail_create?-8:20;
}
static int sceKernelStartThread(int id,int args,void*p){(void)p;assert(id==20&&args==0);starts++;return fail_start?-9:0;}
static int sceKernelDeleteThread(int id){assert(id==20);deletes++;return 0;}
static int sceKernelSignalSema(int id,int count){assert(id==11&&count==1&&g_src_running);signals++;return fail_signal?-7:0;}
static int sceKernelDelayThread(int us){assert(us==10000);return 0;}
static int sceKernelWaitSema(int id,int count,void*p){
 (void)p;assert(id==11&&count==1);waits++;
 if(waits==2){assert(!g_src_running&&!cleanup_live);assert(src_queue_session()==0);queued_during_retirement++;}
 if(waits==3){assert(!g_src_running&&!cleanup_live);longjmp(finished,1);}
 return 0;
}
static int src_thread(SceSize args, void *argp){
 (void)args;(void)argp;assert(g_src_running);cleanup_live=1;sessions++;
 /* Cancel/error does not retire the worker while HTTP/file cleanup is live. */
 g_src_err=1;assert(src_queue_session()<0&&g_src_running);cleanup_live=0;return 0;
}
'''
tail = r'''
int main(void){
 fail_create=1;assert(mv_stream_prepare_worker()==-8&&!g_src_running);fail_create=0;
 fail_start=1;assert(mv_stream_prepare_worker()==-9&&g_src_thid<0&&deletes==1);fail_start=0;
 assert(mv_stream_prepare_worker()==0);int reserved=g_src_thid,c=creates,st=starts;
 assert(mv_stream_prepare_worker()==0&&creates==c&&starts==st);
 assert(!mv_stream_active()&&!mv_stream_worker_active()&&sessions==0);
 fail_signal=1;assert(src_queue_session()==-7&&!g_src_running&&g_src_thid==reserved);fail_signal=0;g_src_err=0;
 assert(src_queue_session()==0&&mv_stream_active()&&mv_stream_worker_active());
 if(!setjmp(finished))src_worker(0,NULL);
 assert(sessions==2&&queued_during_retirement==1&&!mv_stream_worker_active());
 assert(g_src_thid==reserved&&creates==c&&starts==st&&deletes==1);
 puts("PASS production persistent worker: idle/cancel/cleanup, two sessions one UID, retained wake, preparation/start/signal failures");
 return 0;
}
'''
(O / 'test.c').write_text(prefix + s[a:b] + s[a2:b2] + tail, encoding='utf-8')
exe = O / 'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', O / 'test.c', '-o', exe], check=True)
subprocess.run([exe], check=True)
# Structural regression: reservation cannot be mistaken for live source work.
start = s[s.index('int mv_stream_start('):s.index('\nvoid mv_stream_duration(')]
assert 'if (g_src_running) return -1;' in start
assert 'return src_queue_session();' in start
assert 'sceKernelCreateThread' not in start
assert 'sceKernelExitDeleteThread' not in s[a:b]
assert 'g_thread>=0 || g_src_running' in s

# Real-log geometry, not a claim that all physical fragmentation is explained.
# A high persistent allocation at 09DAF300 separates a 1.5MiB upper hole
# from the 12181KiB largest lower hole. Early placement removes that divider.
stack = 0x09DAF300
above = 0x09F3E000
upper_hole = above - (stack + 64 * 1024)
lower_hole = 12181 * 1024
need = 13664 * 1024
assert 1400 * 1024 < upper_hole < 1600 * 1024
assert max(lower_hole, upper_hole) < need
assert lower_hole + upper_hole >= need
print('PASS real-log address model: late stack divides freed arena; early reservation permits sufficient contiguous space (device validation required)')
