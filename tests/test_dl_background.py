"""Execute production download worker against deterministic file/network boundaries."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/dl.c').read_text(encoding='utf-8')
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
# The progress contract is header-only and must travel with the extracted source.
source = '#include "dl_progress_contract.h"\n' + source
preamble = r'''
#include "dl.h"
#include "net.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef APP_DIR
#define APP_DIR "fixture/"
#define PSP_O_RDWR 16
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 4
#define PSP_O_TRUNC 8
#define PSP_SEEK_SET SEEK_SET
#define PSP_SEEK_END SEEK_END
typedef long long SceOff;
typedef unsigned SceSize;
typedef unsigned SceUInt;
static volatile int g_quit;
static FILE *files[32];
static unsigned clock_us=100;
static int opens, queue_reads, stream_reads, open_fail, cancel_on_open, cache_added, marker_fail;
static unsigned sceKernelGetSystemTimeLow(void){return clock_us;}
static void sceKernelDelayThread(int n){clock_us+=n;if(n==2000||n==20000)g_quit=1;}
static int sceKernelWaitSema(int a,int b,void*c){(void)a;(void)b;(void)c;return 0;}
static int sceKernelSignalSema(int a,int b){(void)a;(void)b;return 0;}
static int sceKernelCreateSema(const char*n,int a,int b,int c,void*d){(void)n;(void)a;(void)b;(void)c;(void)d;return 1;}
static int sceKernelCreateThread(const char*n,void*f,int a,int b,int c,void*d){(void)n;(void)f;(void)a;(void)b;(void)c;(void)d;return 1;}
static int sceKernelStartThread(int a,int b,void*c){(void)a;(void)b;(void)c;return 0;}
static int sceKernelWaitThreadEnd(int a,void*b){(void)a;(void)b;return 0;}
static int sceKernelTerminateDeleteThread(int a){(void)a;return 0;}
static int sceKernelDeleteThread(int a){(void)a;return 0;}
static int sceIoOpen(const char*p,int f,int m){(void)m;if(marker_fail&&strstr(p,".part"))return -1;for(int i=1;i<32;i++)if(!files[i]){files[i]=fopen(p,(f&PSP_O_TRUNC)?"w+b":((f&(PSP_O_WRONLY|PSP_O_RDWR))?"r+b":"rb"));if(!files[i]&&(f&PSP_O_CREAT))files[i]=fopen(p,"w+b");return files[i]?i:-1;}return -1;}
static int sceIoClose(int f){int r=fclose(files[f]);files[f]=NULL;return r;}
static int sceIoRemove(const char*p){return remove(p);}
static long long sceIoLseek(int f,SceOff o,int w){if(fseek(files[f],(long)o,w))return -1;return ftell(files[f]);}
static int sceIoWrite(int f,const void*b,int n){return (int)fwrite(b,1,n,files[f]);}
static int sceIoRead(int f,void*b,int n){return (int)fread(b,1,n,files[f]);}
static int sceIoSync(const char*p,int n){(void)p;(void)n;for(int i=1;i<32;i++)if(files[i])fflush(files[i]);return 0;}
void io_lock(void){}
void io_unlock(void){}
static void mlog(const char*f,...){(void)f;}
static void cache_path(char*p,int n,int kind,long long id){(void)kind;snprintf(p,n,APP_DIR "cache/%lld.mp3",id);}
static int cache_has(long long id){(void)id;return 0;}
static void cache_add(const Song*s,int n){(void)s;(void)n;cache_added++;}
static int audio_file_ok(const char*p){(void)p;return 1;}
int net_reconnect_poll(void){return 0;}
void http_close(Http*h){h->sock=-1;}
int http_open_range(Http*h,const char*u,long long o,long long e,int t,char*b,int n){(void)e;(void)t;(void)b;(void)n;opens++;if(open_fail)return -1;h->sock=(!strcmp(u,"queue"))?11:12;h->status=206;h->range_start=o;return 0;}
int http_open_range_cancel(Http*h,const char*u,long long o,long long e,int t,volatile int*c,int gen,char*b,int n){if(cancel_on_open){cancel_on_open=0;dl_stream_stop();assert(c&&*c!=gen);return -3;}return http_open_range(h,u,o,e,t,b,n);}
int http_read(Http*h,char*b,int n,int t,volatile int*c,int g,char*e,int z){(void)t;(void)c;(void)g;(void)e;(void)z;if(h->sock==11)queue_reads++;else stream_reads++;(void)b;(void)n;return -1;}
'''
tail = r'''
static Song song(long long id){Song s;memset(&s,0,sizeof(s));s.id=id;return s;}
static void fixture(void){job_reset(&g_stream);job_reset(&g_qjob);g_quit=0;clock_us=100;}
static void write_prefix(DlJob*j,long long off,int n){j->woff[0]=off;j->wlen[0]=n;memset(g_wbuf[0],0x5a,n);assert(flush_part(j,0)==0);}
static void buffered_stream(void){Song s=song(1);assert(job_begin(&g_stream,&s,"stream",20*DL_CHUNK_SIZE,1,7)==0);advance_chunks(&g_stream,0,5*DL_CHUNK_SIZE);g_stream.done_bytes=5*DL_CHUNK_SIZE;g_stream.watch_t=clock_us;}
int main(int argc,char**argv){assert(argc==2);fixture();
if(!strcmp(argv[1],"background")){buffered_stream();Song s=song(2);assert(dl_enqueue(&s,"queue",20*DL_CHUNK_SIZE)>=0);dl_thread(0,NULL);assert(queue_reads>0);assert(stream_reads>0);assert(g_qjob.nparts==1);}
else if(!strcmp(argv[1],"lowwater")){buffered_stream();g_stream.read_off=5*DL_CHUNK_SIZE;Song s=song(2);assert(dl_enqueue(&s,"queue",20*DL_CHUNK_SIZE)>=0);dl_thread(0,NULL);assert(queue_reads==0);assert(stream_reads>0);}
else if(!strcmp(argv[1],"completed")){buffered_stream();g_stream.complete=1;g_stream.active=0;Song s=song(2);assert(dl_enqueue(&s,"queue",20*DL_CHUNK_SIZE)>=0);dl_thread(0,NULL);assert(queue_reads>0);}
else if(!strcmp(argv[1],"suspend")){Song s=song(2);assert(job_begin(&g_qjob,&s,"queue",20*DL_CHUNK_SIZE,0,0)==0);advance_chunks(&g_qjob,0,4096);g_qjob.done_bytes=4096;suspend_queue();assert(g_qjob.active);assert(g_qjob.fd>=0);assert(job_prefix(&g_qjob)==4096);}
else if(!strcmp(argv[1],"duplicate")){Song s=song(1);assert(dl_stream_start(&s,"stream",900000,7)==0);assert(dl_enqueue(&s,"queue",900000)==-3);}
else if(!strcmp(argv[1],"cancel")){buffered_stream();cancel_on_open=1;assign_parts(&g_stream);assert(!cancel_on_open);assert(!g_stream.error);}
else if(!strcmp(argv[1],"cancelqueued")){Song s=song(2);assert(job_begin(&g_qjob,&s,"queue",900000,0,0)==0);g_cmd_qcancel=1;assign_parts(&g_qjob);assert(opens==0);}
else if(!strcmp(argv[1],"readerpressure")){buffered_stream();Song s=song(2);assert(job_begin(&g_qjob,&s,"queue",900000,0,0)==0);g_read_gen=7;g_read_off=5*DL_CHUNK_SIZE;assign_parts(&g_qjob);assert(opens==0);assert(!g_open_background);}
else if(!strcmp(argv[1],"backoff")){buffered_stream();open_fail=1;assign_parts(&g_stream);int before=opens;for(int i=0;i<30;i++)assign_parts(&g_stream);assert(opens==before);assert(!g_stream.error);}
else if(!strcmp(argv[1],"promote")){Song s=song(2);assert(dl_enqueue(&s,"queue",20*DL_CHUNK_SIZE)>=0);assert(job_begin(&g_qjob,&s,"queue",20*DL_CHUNK_SIZE,0,0)==0);advance_chunks(&g_qjob,0,4096);g_qjob.done_bytes=4096;assert(dl_stream_start(&s,"stream",20*DL_CHUNK_SIZE,8)==0);dl_thread(0,NULL);assert(g_stream.gen==8);assert(job_prefix(&g_stream)==4096);assert(g_queue_n==0);assert(g_qjob.fd<0);}
else if(!strcmp(argv[1],"promotestop")){Song s=song(2);assert(dl_enqueue(&s,"queue",900000)>=0);assert(job_begin(&g_qjob,&s,"queue",900000,0,0)==0);write_prefix(&g_qjob,0,4096);checkpoint_save(&g_qjob);assert(dl_stream_start(&s,"stream",900000,8)==0);dl_thread(0,NULL);dl_stream_stop();g_quit=0;dl_thread(0,NULL);assert(dl_has_song(2));assert(g_queue_n==1);assert(job_prefix(&g_qjob)==4096);}
else if(!strcmp(argv[1],"markerfail")){Song s=song(90);marker_fail=1;assert(job_begin(&g_qjob,&s,"queue",900000,0,0)<0);assert(!g_qjob.active);assert(g_qjob.error);}
else if(!strcmp(argv[1],"capacity")){for(int i=0;i<32;i++){Song s=song(i+10);assert(dl_enqueue(&s,"queue",900000)>=0);}assert(dl_queue_count()==32);}
else if(!strcmp(argv[1],"restart")||!strcmp(argv[1],"sparserestart")||!strcmp(argv[1],"corrupt")||!strcmp(argv[1],"wrongtotal")||!strcmp(argv[1],"wrongurl")){Song s=song(50);assert(job_begin(&g_qjob,&s,"queue",900000,0,0)==0);write_prefix(&g_qjob,0,4096);if(!strcmp(argv[1],"sparserestart"))write_prefix(&g_qjob,2*DL_CHUNK_SIZE,4096);job_close_file(&g_qjob);job_close_net(&g_qjob);if(!strcmp(argv[1],"corrupt")){FILE*f=fopen("fixture/cache/50.mp3","r+b");assert(f);fputc(0,f);fclose(f);}int total=!strcmp(argv[1],"wrongtotal")?800000:900000;const char*u=!strcmp(argv[1],"wrongurl")?"other":"queue";assert(job_begin(&g_qjob,&s,u,total,0,0)==0);int expect=(!strcmp(argv[1],"corrupt")||!strcmp(argv[1],"wrongtotal")||!strcmp(argv[1],"wrongurl"))?0:4096;assert(job_prefix(&g_qjob)==expect);assert(g_qjob.done_bytes==expect);assert(g_qjob.cgot[2]==0);assert(!g_qjob.complete);}
puts("PASS download background/cancellation regression");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='dl-background-') as tmp:
    tmp=Path(tmp);(tmp/'fixture/cache').mkdir(parents=True)
    c=tmp/'test.c';exe=tmp/'test.exe';c.write_text(preamble+source+tail,encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O0','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-I',str(ROOT/'src'),str(c),'-o',str(exe)],check=True)
    failures=[]
    for case in ['background','lowwater','completed','suspend','duplicate','cancel','cancelqueued','readerpressure','backoff','promote','promotestop','markerfail','capacity','restart','sparserestart','corrupt','wrongtotal','wrongurl']:
        case_dir=tmp/case;(case_dir/'fixture/cache').mkdir(parents=True)
        result=subprocess.run([str(exe),case],cwd=case_dir,capture_output=True,text=True,timeout=10)
        print(case,'PASS' if not result.returncode else 'FAIL',result.stderr)
        if result.returncode:failures.append(case)
    assert not failures,failures
