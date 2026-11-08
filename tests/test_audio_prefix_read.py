"""Production audio read must never consume the sparse hole after true prefix."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/audio-prefix-read';O.mkdir(exist_ok=True)
s=(R/'src/audio.c').read_text(encoding='utf-8');a=s.index('static int stream_read(');b=s.index('\nstatic int feed_once',a)
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef long long SceOff;typedef unsigned SceSize;
#define PSP_SEEK_SET 0
#define PSP_O_RDONLY 1
static int g_local,g_gen=3,read_calls,requested,closes;
static const char*g_path="fixture";static long long prefix=100,read_at;
static int dl_stream_readable(int gen,long long off,int want){assert(gen==3);if(off>=prefix)return 0;long long n=prefix-off;return n<want?(int)n:want;}
static long long sceIoLseek(int fd,SceOff at,int mode){(void)fd;(void)mode;read_at=at;return at;}
static int sceIoRead(int fd,void*b,SceSize n){(void)fd;read_calls++;requested=(int)n;assert(g_local||read_at+n<=prefix);memset(b,0x5a,n);return n;}
static int sceIoSync(const char*p,int mode){(void)p;(void)mode;return 0;}
static int sceIoOpen(const char*p,int mode,int n){(void)p;(void)mode;(void)n;return 4;}
static int sceIoClose(int fd){(void)fd;closes++;return 0;}
#define mlog(...) ((void)0)
'''
tail=r'''
int main(void){int fd=3;char data[64];assert(stream_read(&fd,80,data,64)==20&&requested==20&&read_calls==1);assert(stream_read(&fd,100,data,64)==0&&read_calls==1);assert(stream_read(&fd,200,data,64)==0&&read_calls==1);prefix=300;assert(stream_read(&fd,200,data,64)==64&&read_calls==2);g_local=1;assert(stream_read(&fd,400,data,64)==64);puts("PASS production audio read: clamp true written prefix, holes never read, later prefix available, local unaffected");return 0;}
'''
(O/'test.c').write_text(prefix+s[a:b]+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
