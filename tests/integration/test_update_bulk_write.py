"""Exercise production updater with 1KiB arrivals; writes should coalesce without losing cancellation or integrity."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];O=R/'.tmp/update-bulk-write';O.mkdir(exist_ok=True)
s=(R/'src/api.c').read_text(encoding='utf-8');a=s.index('static void job_update_download(');b=s.index('\n/* ------------------------------------------------------------------ */',a)
prefix=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include "update_pbp.h"
#define APP_DIR ""
#define UPDATE_NEW "EBOOT.NEW"
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 512
#define PSP_O_TRUNC 1024
typedef struct{long long content_length;int status;}Http;
typedef struct{long long st_size;}SceIoStat;
static int g_upd_state=7,g_upd_progress;static volatile int g_upd_abort;static char g_upd_msg[160],g_upd_url[128]="http://fixture/",g_mode[24];static long long g_upd_size;
static unsigned char*payload;static int length,at,body_writes,body_fd=-1;static FILE*files[16];static unsigned now;
static unsigned sceKernelGetSystemTimeLow(void){return now+=1000;}
static void mlog(const char*fmt,...){(void)fmt;}
static void sceKernelDelayThread(unsigned us){assert(us<=1000);}
static int http_open(Http*h,const char*u,long long from,int ms,char*err,int en){(void)u;(void)from;(void)ms;(void)err;(void)en;h->content_length=length;h->status=200;return 0;}
static int http_read(Http*h,char*b,int n,int ms,volatile int*abort,int gen,char*e,int en){(void)h;(void)ms;(void)gen;(void)e;(void)en;if(*abort)return -2;if(!strcmp(g_mode,"cancel")&&at>=100000){g_upd_abort=1;return -2;}if(!strcmp(g_mode,"truncated")&&at>=length-4096)return 0;if(at==length)return 0;int k=length-at;if(k>1024)k=1024;if(k>n)k=n;memcpy(b,payload+at,k);at+=k;return k;}
static void http_close(Http*h){(void)h;}
static int sceIoGetstat(const char*p,SceIoStat*s){FILE*f=fopen(p,"rb");if(!f)return -1;fseek(f,0,SEEK_END);s->st_size=ftell(f);fclose(f);return 0;}
static int sceIoRemove(const char*p){return remove(p);}
static int sceIoOpen(const char*p,int mode,int perm){(void)perm;FILE*f=fopen(p,mode&2?"wb":"rb");if(!f)return -1;for(int i=1;i<16;i++)if(!files[i]){files[i]=f;if(!strcmp(p,UPDATE_NEW)&&(mode&2))body_fd=i;return i;}abort();}
static int sceIoRead(int fd,void*b,int n){return (int)fread(b,1,n,files[fd]);}
static int sceIoWrite(int fd,const void*b,int n){if(fd==body_fd)body_writes++;if(!strcmp(g_mode,"writefail")&&body_writes==2)return -1;if(!strcmp(g_mode,"shortwrite")&&n>2048)n=2048;return (int)fwrite(b,1,n,files[fd]);}
static int sceIoClose(int fd){if(fd==body_fd)body_fd=-1;int r=fclose(files[fd]);files[fd]=NULL;return r;}
static int sceIoSync(const char*p,int mode){(void)p;(void)mode;return 0;}
static int io_write_complete(int fd,const void*b,int n){int at=0;while(at<n){int r=sceIoWrite(fd,(const char*)b+at,n-at);if(r<=0)return -1;at+=r;}return 0;}
'''
tail=r'''
int main(int argc,char**argv){assert(argc==3);snprintf(g_mode,sizeof(g_mode),"%s",argv[2]);FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);length=(int)ftell(f);fseek(f,0,SEEK_SET);payload=malloc(length);assert(fread(payload,1,length,f)==(unsigned)length);fclose(f);g_upd_size=length;job_update_download();
if(!strcmp(g_mode,"normal")||!strcmp(g_mode,"shortwrite")){assert(g_upd_state==5&&g_upd_progress==100);f=fopen(UPDATE_NEW,"rb");assert(f);unsigned char*b=malloc(length+1);assert(fread(b,1,length+1,f)==(unsigned)length&&!memcmp(b,payload,length));fclose(f);free(b);if(!strcmp(g_mode,"normal")){int expected=(length+65535)/65536;printf("actual body writes=%d expected at most=%d for %d bytes at1KiB arrivals\n",body_writes,expected,length);assert(body_writes<=expected);}puts("PASS complete exact PBP, full size/meta validation, aggregated or short writes");}
else{assert(g_upd_state==(!strcmp(g_mode,"cancel")?0:6));assert(!fopen(UPDATE_NEW,"rb"));assert(!fopen("update.meta","rb"));puts("PASS cancelled/incomplete/write-failed download never leaves installable update");}free(payload);return 0;}
'''
(O/'test.c').write_text(prefix+s[a:b]+tail,encoding='utf-8');exe=O/'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-unused-function','-I'+str(R/'src'),O/'test.c','-o',exe],check=True)
pbp=R/'releases/MELODY-3.4.2/在线更新/EBOOT.PBP'
for mode in ['normal','shortwrite','cancel','truncated','writefail']:
 with tempfile.TemporaryDirectory(prefix='melody-bulk-')as t:subprocess.run([exe,pbp,mode],cwd=t,check=True)
