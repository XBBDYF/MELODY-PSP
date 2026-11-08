"""Verify actual download completion publishes stable metadata and MV index."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/mv-download-publish';O.mkdir(exist_ok=True)
s=(R/'src/mv.c').read_text(encoding='utf-8');a=s.index('static void src_mark_done(');b=s.index('\nstatic void src_download_finished(',a)
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned SceSize;
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 512
#define PSP_O_TRUNC 1024
static char g_src_final[160]="fixture/mv_12_240.mp4",g_src_name[72]="Download title",g_src_artist[64]="Download artist";
static int g_src_duration_ms=90000,g_src_mvid=12,index_calls,index_id,index_size;
static long long g_src_total=123456;
static char written[220],opened[160];
static int sceIoOpen(const char*p,int f,int m){(void)f;(void)m;snprintf(opened,sizeof(opened),"%s",p);return 3;}
static int sceIoWrite(int fd,const void*p,unsigned n){assert(fd==3&&n<sizeof(written));memcpy(written,p,n);written[n]=0;return n;}
static int sceIoClose(int fd){assert(fd==3);return 0;}
static void mv_index_add(int id,int bytes){index_calls++;index_id=id;index_size=bytes;}
'''
tail=r'''
int main(void){src_mark_done();assert(!strcmp(opened,"fixture/mv_12_240.txt"));assert(!strcmp(written,"Download title\nDownload artist\n90000\n"));assert(index_calls==1&&index_id==12&&index_size==123456);g_src_total=0x80000000LL;src_mark_done();assert(index_size==0x7fffffff);g_src_mvid=0;src_mark_done();assert(index_calls==2);puts("PASS actual completion metadata snapshot and MV index publication; oversized count clamp");return 0;}
'''
(O/'test.c').write_text(prefix+s[a:b]+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
