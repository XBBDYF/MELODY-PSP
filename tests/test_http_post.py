"""Production POST transport admission and response handling, no cloud mutations."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/http-post-test';O.mkdir(exist_ok=True)
s=(R/'src/net.c').read_text(encoding='utf-8');a=s.index('int http_post_form(');b=s.index('/* 2026-11-01：api 线程被强杀后',a)
prefix=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include "net.h"
typedef int SceUID;
static int g_get_lock=-1,post_owner=-1,open_calls,read_calls,closed,signaled,status=200,fail_read;
static const char*post_body;static Http g_get_http;
static unsigned now;
static unsigned sceKernelGetSystemTimeLow(void){return now+=10000;}
static int sceKernelGetThreadId(void){return 44;}
static int sceKernelCreateSema(const char*n,int a,int b,int c,void*d){(void)n;(void)a;(void)b;(void)c;(void)d;return 7;}
static int sceKernelWaitSema(int s,int n,void*t){(void)s;(void)n;(void)t;return 0;}
static int sceKernelSignalSema(int s,int n){(void)s;(void)n;signaled++;return 0;}
static int aborted(void){return 0;}
static int http_connect_once(Http*h,const char*u,long long f,long long t,int tm,int bt,char*e,int en,char*l,int ln){(void)u;(void)f;(void)t;(void)tm;(void)bt;(void)e;(void)en;(void)l;(void)ln;assert(h->no_reuse&&post_owner==44&&!strcmp(post_body,"id=123&t=1"));h->status=status;open_calls++;return 0;}
static int http_read_stub(Http*h,char*b,int n,int idle,void*g,int gen,char*e,int en){(void)h;(void)idle;(void)g;(void)gen;(void)e;(void)en;read_calls++;if(fail_read)return -2;if(read_calls==1){const char*v="{\"code\":200}";int k=(int)strlen(v);assert(n>=k);memcpy(b,v,k);return k;}return 0;}
#define http_read http_read_stub
static void http_close_stub(Http*h){assert(h->reused_broken);closed++;}
#define http_close http_close_stub
'''
tail=r'''
static void reset(void){open_calls=read_calls=closed=signaled=0;status=200;fail_read=0;}
int main(void){char out[128],err[80];int n;reset();assert(http_post_form("http://fixture/subscribe","id=123&t=1",out,sizeof(out),&n,2000,err,sizeof(err))==0);assert(n==12&&!strcmp(out,"{\"code\":200}")&&closed==1&&signaled==1&&!post_body&&post_owner==-1);reset();status=302;assert(http_post_form("http://fixture/subscribe","id=123&t=1",out,sizeof(out),&n,2000,err,sizeof(err))<0&&open_calls==1&&closed==1&&read_calls==0);reset();fail_read=1;assert(http_post_form("http://fixture/subscribe","id=123&t=1",out,sizeof(out),&n,2000,err,sizeof(err))<0&&closed==1&&signaled==1);reset();assert(http_post_form("https://fixture/subscribe","id=123&t=1",out,sizeof(out),&n,2000,err,sizeof(err))<0&&open_calls==0);puts("PASS production POST form: confirmed response, no redirects/retries, read failure closes/unlocks, invalid scheme refused");return 0;}
'''
(O/'test.c').write_text(prefix+s[a:b]+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(R/'src'),O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
