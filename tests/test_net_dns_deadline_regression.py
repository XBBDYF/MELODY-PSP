"""The SDK resolver can block for tens of seconds, so it runs on a worker and the
caller only waits DNS_JOB_DEADLINE_MS.

Field logs: first lookup 11.2s, another session never returned (46s, power off).
sceNetResolverStartNtoA is synchronous and its retry/timeout arguments did not bound
it, so the deadline has to live in our code: submit -> publish by generation -> wait
with a cap. These asserts pin that contract.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/net.c").read_text(encoding="utf-8")


def slice_between(begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]


worker = slice_between("static int dns_worker_step(void)", "static int dns_worker_main(")
wait_job = slice_between("static int dns_wait_job(", "static int dns_resolve_bounded(")

PRE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct in_addr { unsigned int s_addr; };
static int lookup_result, creates, starts, deletes;
static unsigned now;
static unsigned sceKernelGetSystemTimeLow(void){return now;}
static unsigned sceKernelCpuSuspendIntr(void){return 0;}
static void sceKernelCpuResumeIntr(unsigned irq){(void)irq;}
static void sceKernelDelayThread(unsigned us){now+=us;}
static int aborted(void){return 0;}
static void mlog(const char *format,...){(void)format;}
static volatile int g_dns_state;
static volatile int g_dns_gen, g_dns_done_gen;
static volatile unsigned int g_dns_ip;
static char g_dns_host[128];
static unsigned ip_pack(const unsigned char *b){
    return (unsigned)b[0]<<24 | (unsigned)b[1]<<16 | (unsigned)b[2]<<8 | b[3];
}
static int sceNetResolverCreate(int *id,void *buffer,unsigned size){
    assert(buffer && size>=1024);creates++;*id=17;return creates>2 ? -456 : 0;
}
static int sceNetResolverStartNtoA(int id,const char *host,struct in_addr *address,unsigned timeout,int retries){
    (void)host;assert(id==17);assert(timeout==2 && retries==2);starts++;
    unsigned char bytes[4]={192,0,2,7};memcpy(&address->s_addr,bytes,4);
    return lookup_result;
}
static int sceNetResolverDelete(int id){assert(id==17);deletes++;return 0;}
'''
MAIN = r'''
int main(void){
    unsigned ip=0;
    /* 1) success publishes bytes and the generation that produced them. */
    g_dns_gen=1;snprintf(g_dns_host,sizeof(g_dns_host),"%s","resolver.invalid");
    lookup_result=0;
    assert(dns_worker_step()==0);
    assert(g_dns_state==2 && g_dns_done_gen==1 && g_dns_ip==0xc0000207u);
    assert(creates==1 && starts==1 && deletes==1);
    assert(dns_wait_job(1,2500,&ip)==0 && ip==0xc0000207u);

    /* 2) a stale generation is never accepted as this job's answer. */
    assert(dns_wait_job(2,50,&ip)==-1);

    /* 3) resolver failure publishes failure for its own generation only. */
    g_dns_gen=3;lookup_result=-123;
    assert(dns_worker_step()==-1);
    assert(g_dns_state==3 && g_dns_done_gen==3 && dns_wait_job(3,100,&ip)==-1);

    /* 4) a job that never completes is abandoned at the deadline. */
    g_dns_gen=4;g_dns_state=1;now=0;
    assert(dns_wait_job(4,2500,&ip)==-1);
    assert(now>=2500000u);

    /* 5) resolver creation failure is a failed job, not a hang. */
    g_dns_gen=5;/* creates becomes 3 -> mock returns -456 */
    assert(dns_worker_step()==-1 && g_dns_done_gen==5);
    assert(creates==3 && starts==2 && deletes==2);
    puts("PASS bounded DNS worker: generation-scoped results, failure publish, 2.5s abandon, create failure");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="net-dns-deadline-") as directory:
    directory = Path(directory)
    c_file = directory / "dns.c"
    executable = directory / "dns.exe"
    c_file.write_text(PRE + worker + wait_job + MAIN, encoding="utf-8")
    subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O0", "-Wall", "-Wextra",
                    "-Wno-unused-function", "-Werror", str(c_file), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
