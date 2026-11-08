"""Exercise the production range-open scope and TCP select loop on a host."""
from pathlib import Path
import subprocess
import tempfile
R=Path(__file__).resolve().parents[1]
s=(R/'src/net.c').read_text(encoding='utf-8')
def section(a,b):return s[s.index(a):s.index(b,s.index(a))]
state=section('static volatile int *g_abort_flag;','/* 全局 Cookie')
abort=section('void http_set_abort(','/* ------------------------------------------------------------------ */')
tcp=section('static int connect_errno_is(','static int tcp_connect(')
wrapper=section('int http_open_range_cancel(','/* 大文件整包下载（MV）')
pre=r'''
#include <winsock2.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
typedef int SceUID;
typedef int socklen_t;
typedef struct {int sock;} Http;
#undef FD_ISSET
#define FD_ISSET(s,f) ((void)(s),(f)->fd_count>0)
#define SO_NONBLOCK 0x1009
static int thread_id=1,closed,selects;
static unsigned clock_us;
static volatile int generation=7;
static int sceKernelGetThreadId(void){return thread_id;}
static unsigned sceKernelGetSystemTimeLow(void){return clock_us;}
static void sceKernelDelayThread(int us){clock_us+=us;}
static void mlog(const char*f,...){(void)f;}
static void net_drop_idle(void){}
static void ip_unpack(unsigned int p,unsigned char*b){memcpy(b,&p,4);}
static void set_timeouts(int s,int a,int b){(void)s;(void)a;(void)b;}
static int fake_socket(int a,int b,int c){(void)a;(void)b;(void)c;return 3;}
static int fake_setsockopt(int s,int l,int o,const void*v,int n){(void)s;(void)l;(void)o;(void)v;(void)n;return 0;}
static int fake_connect(int s,const struct sockaddr*a,int n){(void)s;(void)a;(void)n;errno=EINPROGRESS;return -1;}
static int fake_select(int n,fd_set*r,fd_set*w,fd_set*e,struct timeval*t){(void)n;(void)r;(void)w;(void)e;clock_us+=(unsigned)t->tv_usec;selects++;generation++;return 0;}
static int fake_getsockopt(int s,int l,int o,void*v,socklen_t*n){(void)s;(void)l;(void)o;(void)v;(void)n;return -1;}
static int fake_getpeername(int s,struct sockaddr*a,socklen_t*n){(void)s;(void)a;(void)n;return -1;}
static int fake_close(int s){(void)s;closed++;return 0;}
static unsigned short fake_short(unsigned short n){return n;}
#define socket fake_socket
#define setsockopt fake_setsockopt
#define connect fake_connect
#define select fake_select
#define getsockopt fake_getsockopt
#define getpeername fake_getpeername
#define close fake_close
#define htons fake_short
#define ntohs fake_short
'''
middle=r'''
static int http_open_full(Http*h,const char*u,long long f,long long t,int tm,int bt,char*e,int n){(void)u;(void)f;(void)t;(void)bt;(void)e;(void)n;h->sock=try_connect(0x0100007f,80,tm,65536);return h->sock<0?-1:0;}
static void http_close(Http*h){if(h->sock>=0){fake_close(h->sock);h->sock=-1;}}
'''
main=r'''
int main(void){volatile int api_abort=1;http_set_abort(&api_abort);assert(aborted());thread_id=2;Http h;char err[80];assert(http_open_range_cancel(&h,"http://fixture/",0,100,8000,&generation,7,err,sizeof(err))==-3);assert(selects==1);assert(clock_us==50000);assert(closed==1);assert(h.sock==-1);assert(g_cancel_owner==-1);assert(!aborted());thread_id=1;assert(aborted());puts("PASS actual range scope + TCP cancellation in one 50ms select; API abort unchanged");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='net-connect-cancel-') as t:
    t=Path(t);c=t/'test.c';e=t/'test.exe';c.write_text(pre+state+abort+tcp+middle+wrapper+main,encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O0','-Wall','-Wextra','-Werror','-Wno-unused-function',c,'-o',e],check=True)
    subprocess.run([e],check=True)
