"""Fault injection at SDK boundary: real fd registry and reset cannot hang.
Owner metadata is deliberately set to reproduce a worker killed inside close.
"""
from pathlib import Path
import subprocess
import tempfile
from net_fixture import function

R = Path(__file__).resolve().parents[1]
s = (R / 'src/net.c').read_text(encoding='utf-8')
assert 'g_fd_owner' in s, 'fd semaphore needs owner-aware bounded recovery'
a = s.index('static unsigned char g_active_fd[FD_SETSIZE];')
b = s.index('static void dialog_retire_network(', a)
registry = s[a:b]
pre = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define FD_SETSIZE 16
#define SHUT_RDWR 2
#define PSP_THREAD_STOPPED 16
#define PSP_THREAD_KILLED 32
#define SCE_KERNEL_ERROR_UNKNOWN_THID ((int)0x80020198)
#define SCE_KERNEL_ERROR_UNKNOWN_UID ((int)0x800200cb)
typedef int SceUID;
typedef unsigned SceUInt;
typedef struct {unsigned size;unsigned status;} SceKernelThreadInfo;
static int count[64],next_sema=10,waits,deletes,query_error,thread_status,closes,sockets;
static unsigned waited;
static int sceKernelGetThreadId(void){return 3;}
static unsigned sceKernelCpuSuspendIntr(void){return 0;}
static void sceKernelCpuResumeIntr(unsigned x){(void)x;}
static int sceKernelWaitSema(int id,int n,SceUInt *timeout){assert(timeout && *timeout<=100000);waits++;if(count[id]>=n){count[id]-=n;return 0;}waited+=*timeout;return -1;}
static int sceKernelSignalSema(int id,int n){count[id]+=n;return 0;}
static int sceKernelCreateSema(const char*n,int a,int initial,int max,void*o){(void)n;(void)a;(void)max;(void)o;count[next_sema]=initial;return next_sema++;}
static int sceKernelDeleteSema(int id){count[id]=0;deletes++;return 0;}
static int sceKernelReferThreadStatus(int id,SceKernelThreadInfo *info){assert(id==7);assert(info->size==sizeof(*info));info->status=thread_status;return query_error;}
static void mlog(const char*f,...){(void)f;}
static int network_blocked(void){return 0;}
static int socket(int d,int t,int p){(void)d;(void)t;(void)p;sockets++;return 4;}
static int close(int fd){assert(fd==4);closes++;return 0;}
static int shutdown(int fd,int how){assert(fd==4&&how==2);return 0;}
'''
post = r'''
typedef struct {int sock,reused_broken;} Http;
static Http g_get_http={4,0};
static Http *g_connect_http;
static const char *post_body;
static int post_owner,g_want_gzip,g_want_keepalive,g_get_lock=-1,g_conn_lock=-1;
static void http_close(Http*h){if(h->sock>=0){tracked_close(h->sock);h->sock=-1;}}
'''
main = r'''
int main(void){
 g_fd_lock=1;g_fd_owner=7;count[1]=0;thread_status=1;
 assert(!fd_lock());assert(waited<=100000&&deletes==0&&g_fd_lock==1);
 waited=0;thread_status=PSP_THREAD_STOPPED;
 net_reset_locks();assert(waited<=200000&&deletes==1);assert(closes==1);
 assert(tracked_socket(2,1,0)==4);assert(sockets==1);
 shutdown_active();assert(tracked_close(4)==0);
 /* Deleted owner recovers, arbitrary SDK query error preserves original lock. */
 g_fd_lock=2;g_fd_owner=7;count[2]=0;query_error=-999;waited=0;
 assert(!fd_lock());assert(g_fd_lock==2&&deletes==1&&waited<=100000);
 query_error=SCE_KERNEL_ERROR_UNKNOWN_THID;waited=0;
 int acquired=fd_lock();assert(deletes==2&&g_fd_lock!=2&&waited<=200000);
 if(acquired)fd_unlock();
 assert(tracked_socket(2,1,0)==4);assert(tracked_close(4)==0);
 puts("PASS fd lock: alive owner retained; bounded reset recovers stopped/deleted owner; later socket works; unknown query errors preserve lock");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='net-fd-recovery-') as directory:
    directory = Path(directory)
    c = directory / 'recovery.c'
    exe = directory / 'recovery.exe'
    c.write_text(pre + registry + post + function(s, 'net_reset_locks').replace('int net_reset_locks(', 'void net_reset_locks(') + main, encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
