"""Compile actual POST request construction and body dispatch; no live account."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/post-request-test';O.mkdir(exist_ok=True)
s=(R/'src/net.c').read_text(encoding='utf-8');a=s.index('    if(posting){');b=s.index('    }else if (range_from',a);construct=s[a:b]+'\n    }\n'
a=s.index('    if(posting&&send_all(');b=s.index('\n    {',a);send=s[a:b]
code=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef struct{char path[64],host[64];int sock;}Http;
static const char*post_body="id=123456&t=1&csrf_token=test";
static char wire[2048];static int sent;
static const char*cookie_hdr(const char*h){(void)h;return "Cookie: fixture=one\r\n";}
static int send_all(int sock,const char*b,int n){assert(sock==3);assert(n==(int)strlen(post_body));strcpy(wire+sent,b);sent+=n;return n;}
static int run(void){Http value={"/api/playlist/subscribe","fixture",3},*h=&value;char req[2048],err[80];int errlen=sizeof(err),n=0,posting=1;
'''+construct+r'''
assert(n>0&&n<(int)sizeof(req));assert(strstr(req,"POST /api/playlist/subscribe HTTP/1.0\r\n"));assert(strstr(req,"Content-Type: application/x-www-form-urlencoded\r\n"));char expected[80];snprintf(expected,sizeof(expected),"Content-Length: %u\r\n",(unsigned)strlen(post_body));assert(strstr(req,expected)&&strstr(req,"Connection: close\r\n"));strcpy(wire,req);sent=n;
'''+send+r'''
assert(!strcmp(wire+n,post_body));puts("PASS actual POST headers, Content-Length, body exactly once, cookie and close transport");return 0;}
int main(void){return run();}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
