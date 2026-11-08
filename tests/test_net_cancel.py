"""Run production abort ownership and header-wait cancellation on a host."""
from pathlib import Path
import subprocess
import tempfile
R=Path(__file__).resolve().parents[1]
s=(R/'src/net.c').read_text(encoding='utf-8')
# Keep the actual abort state/functions, not a behavioral copy.
state=s[s.index('static volatile int *g_abort_flag;'):s.index('/* 全局 Cookie')]
fn=s[s.index('void http_set_abort('):s.index('/* ------------------------------------------------------------------ */',s.index('void http_set_abort('))]
recv=s[s.index('static int recv_wait('):s.index('static int send_all(')]
pre=r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
typedef int SceUID;
static int thread_id=1, calls;
static unsigned clock_us;
static volatile int generation=7;
static int sceKernelGetThreadId(void){return thread_id;}
static unsigned sceKernelGetSystemTimeLow(void){return clock_us;}
static void sceKernelDelayThread(int n){clock_us+=n;generation=8;}
#define MSG_DONTWAIT 1
static int recv(int s,char*b,int n,int f){(void)s;(void)b;(void)n;(void)f;calls++;errno=EAGAIN;return -1;}
static void net_received(int n){(void)n;}
'''
# On the baseline, own generation context does not exist; use its normal
# global abort behavior to expose the missing isolation first.
if 'g_cancel_owner' in state:
    setup='g_cancel_owner=2;g_cancel_flag=&generation;g_cancel_value=7;'
else: setup=''
main=r'''
int main(void){volatile int flag=1;http_set_abort(&flag);assert(aborted());thread_id=2;assert(!aborted());SETUP
char b[8];assert(recv_wait(1,b,8,8000)<0);assert(clock_us<100000);assert(calls<10);thread_id=1;assert(aborted());puts("PASS thread-owned abort and prompt header cancellation");return 0;}
'''.replace('SETUP',setup)
with tempfile.TemporaryDirectory(prefix='net-cancel-') as t:
    t=Path(t);c=t/'test.c';e=t/'test.exe';c.write_text(pre+state+fn+recv+main,encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',c,'-o',e],check=True)
    subprocess.run([e],check=True)
