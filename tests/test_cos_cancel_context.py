"""Run production TCP entry: offline COS reconnect precedes DNS; abort wins."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/net.c').read_text(encoding='utf-8')
start = source.index('static int tcp_connect(')
code = source[start:source.index('int net_probe_host(', start)]
pre = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define PSP_NET_APCTL_STATE_GOT_IP 4
static int cancelled, state=0, kicks, dns_calls, connects, g_net_ok=1;
static int g_last_profile=4;
static int aborted(void){return cancelled;}
static int net_apctl_state(void){return state;}
static int net_reconnect_poll(void){kicks++;return -1;}
static int net_current_ip(char *p,int n){snprintf(p,n,"0.0.0.0");return 0;}
static unsigned sceKernelGetSystemTimeLow(void){return 0;}
static void sceKernelDelayThread(int n){(void)n;}
static void mlog(const char *f,...){(void)f;}
static int dns_lookup(const char*h,unsigned*i,int*c){(void)h;*i=1;if(c)*c=0;dns_calls++;return 0;}
static void dns_expire(const char*h){(void)h;}
static int try_connect(unsigned i,int p,int t,int b){(void)i;(void)p;(void)t;(void)b;connects++;return 7;}
static int connect_errno_is(int v,int c){return v==c;}
'''
main = r'''
int main(void){char err[128];
assert(tcp_connect("cos.fixture",80,9000,0,err,sizeof(err))<0);
assert(kicks==1 && dns_calls==0 && connects==0);
assert(errno==EADDRNOTAVAIL && strstr(err,"state=0"));
cancelled=1;
assert(tcp_connect("cos.fixture",80,9000,0,err,sizeof(err))<0);
assert(errno==ECANCELED && kicks==1 && dns_calls==0);
cancelled=0;state=4;
assert(tcp_connect("cos.fixture",80,9000,0,err,sizeof(err))==7);
assert(kicks==1 && dns_calls==1 && connects==1);
puts("PASS offline COS reconnect before DNS; explicit link error; cancellation; connected path");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='cos-cancel-context-') as directory:
    path = Path(directory)
    cfile, binary = path/'test.c', path/'test.exe'
    cfile.write_text(pre+code+main, encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',str(cfile),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
