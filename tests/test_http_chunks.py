import pathlib, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[1]
out=root/'.tmp/chunk-host-regression'; out.mkdir(exist_ok=True)
s=(root/'src/net.c').read_text(encoding='utf-8-sig')
if '--save-old' in sys.argv: (out/'net_old.c').write_text(s,encoding='utf-8')
if '--old' in sys.argv: s=(out/'net_old.c').read_text(encoding='utf-8')
def section(a,b): return s[s.index(a):s.index(b,s.index(a))]
code=r'''
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include "E:/MELODY_MV_20261102/src/net.h"
static const unsigned char *wire; static int wlen,pos,split,paused,onebyte,timeout_each,pending_timeout,terminal,puts_count,closes_count; static unsigned clock_us;
static unsigned sceKernelGetSystemTimeLow(void){return clock_us+=1000;}
static void sceKernelDelayThread(int us){clock_us+=us;}
static void net_received(int n){(void)n;}
static int aborted(void){return 0;}
#define MSG_DONTWAIT 1
static int recv(int sock,char *b,int len,int flags){
 (void)sock;(void)flags;
 if(pending_timeout){pending_timeout=0;errno=EAGAIN;return -1;}
 if(!paused && split>=0 && pos==split){paused=1;errno=EAGAIN;return -1;}
 if(pos==wlen){if(terminal<0){errno=terminal==-1?EAGAIN:EIO;return -1;}return 0;}
 if(onebyte && len>1)len=1;
 if(split>=0 && !paused && pos<split && len>split-pos)len=split-pos;
 if(len>wlen-pos)len=wlen-pos;
 memcpy(b,wire+pos,len);pos+=len;if(timeout_each)pending_timeout=1;return len;
}
static void ka_put(const char*h,int p,int sock){(void)h;(void)p;(void)sock;puts_count++;}
static int g_offline,g_dialog_reserved;
static int tracked_close(int sock){(void)sock;closes_count++;return 0;}
'''
code+=section('static int recv_wait(', 'static int send_all(')
code+=section('static int fill(', '/* ------------------------------------------------------------------ */\n/* HTTP')
code+=section('void http_close(', '/* 静态 Http：')
code+=r'''
static int failures,checks,read_quantum;
#define CHECK(c,label) do{checks++;if(!(c)){printf("FAIL %s (line %d)\n",label,__LINE__);failures++;}}while(0)
static void setup(Http*h,const unsigned char*s,int n,int cut,int every,int end){memset(h,0,sizeof(*h));h->sock=1;h->chunked=1;h->keep_alive=1;h->status=200;wire=s;wlen=n;pos=0;split=cut;paused=0;onebyte=every;timeout_each=every;pending_timeout=0;terminal=end;puts_count=closes_count=0;}
static int drain(Http*h,char*out,int cap,char*err){int total=0;for(int i=0;i<100000;i++){int room=cap-total;if(read_quantum && room>read_quantum)room=read_quantum;int n=http_read(h,out+total,room,0,NULL,-1,err,128);if(n>0){total+=n;if(total>=cap)return -99;}else if(n==-1)continue;else{out[total]=0;return n<0?n:total;}}return -98;}
int main(void){
 Http h;char out[4096],err[128];int n;
 const unsigned char json[]="3\r\n{}\n\r\n0\r\n\r\n";err[0]=0;setup(&h,json,sizeof(json)-1,1,0,0);n=drain(&h,out,sizeof(out),err);if(n!=3)printf("root reproducer: return=%d err=%s\n",n,err);CHECK(n==3 && !memcmp(out,"{}\n",3),"size timeout must not parse JSON body as header");
 const unsigned char normal[]="10\r\n0123456789abcdef\r\n0\r\n\r\n";
 setup(&h,normal,sizeof(normal)-1,1,0,0);n=drain(&h,out,sizeof(out),err);CHECK(n==16 && !memcmp(out,"0123456789abcdef",16),"size prefix retained across timeout");
 const unsigned char ext[]="4;name=\"a;b\"\r\nWiki\r\n5;foo=bar\r\npedia\r\n0\r\nX-One: a\r\nX-Two: b\r\n\r\n";
 setup(&h,ext,sizeof(ext)-1,-1,1,0);n=drain(&h,out,sizeof(out),err);CHECK(n==9 && !memcmp(out,"Wikipedia",9),"one-byte timeouts extension trailer CRLF");CHECK(h.finished,"finished only after all trailers");CHECK(pos==wlen,"all trailer bytes consumed");http_close(&h);CHECK(puts_count==1 && closes_count==0,"complete response reusable");
 for(int i=0;i<(int)sizeof(ext)-1;i++){setup(&h,ext,sizeof(ext)-1,i,0,0);n=drain(&h,out,sizeof(out),err);CHECK(n==9 && !memcmp(out,"Wikipedia",9),"every single split retains framing");}
 const char *bad[]={"x\r\n","1z\r\nx\r\n0\r\n\r\n","80000000\r\n","100000000\r\n","1\nx\r\n0\r\n\r\n","1\r\nx!\r\n0\r\n\r\n","\r\n","0\r\nX: x\r\n"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){setup(&h,(const unsigned char*)bad[i],strlen(bad[i]),-1,0,0);n=drain(&h,out,sizeof(out),err);CHECK(n==-2,"malformed or truncated framing rejected");CHECK(!h.finished,"invalid response never finished");http_close(&h);CHECK(!puts_count && closes_count==1,"invalid response not pooled");}
 const unsigned char nul[]={ '1',0,'\r','\n','x','\r','\n','0','\r','\n','\r','\n'};
 setup(&h,nul,sizeof(nul),-1,0,0);n=drain(&h,out,sizeof(out),err);CHECK(n==-2 && !h.finished,"NUL is invalid byte not EOF");
 for(int i=0;i<(int)sizeof(normal)-2;i++){setup(&h,normal,i,-1,0,0);n=drain(&h,out,sizeof(out),err);CHECK(n==-2 && !h.finished,"EOF at every incomplete byte rejected");}
 setup(&h,normal,1,-1,0,-2);n=drain(&h,out,sizeof(out),err);CHECK(n==-2 && !h.finished,"hard recv error distinct from retry");
 setup(&h,normal,1,-1,0,-1);n=http_read(&h,out,sizeof(out),0,NULL,-1,err,sizeof(err));CHECK(n==-1 && !h.finished,"temporary timeout not EOF");http_close(&h);CHECK(!puts_count,"partial timeout not pooled");
 setup(&h,(const unsigned char*)"0\r\nX: a",7,-1,0,-1);n=http_read(&h,out,sizeof(out),0,NULL,-1,err,sizeof(err));CHECK(n==-1 && !h.finished,"partial trailer waits");http_close(&h);CHECK(!puts_count,"partial trailer not reusable");
 char huge[20000];memset(huge,'a',sizeof(huge));memcpy(huge,"1;",2);setup(&h,(unsigned char*)huge,sizeof(huge),-1,0,-1);n=http_read(&h,out,sizeof(out),0,NULL,-1,err,sizeof(err));CHECK(n==-2 && h.bpos==1025,"overlong size extension bounded");
 memset(huge,'a',sizeof(huge));memcpy(huge,"0\r\nX: ",6);setup(&h,(unsigned char*)huge,sizeof(huge),-1,0,-1);n=http_read(&h,out,sizeof(out),0,NULL,-1,err,sizeof(err));CHECK(n==-2 && !h.finished,"overlong trailer bounded");
 char many[20010];int ml=3;memcpy(many,"0\r\n",3);for(int i=0;i<4000;i++){memcpy(many+ml,"X:a\r\n",5);ml+=5;}setup(&h,(unsigned char*)many,ml,-1,0,0);n=drain(&h,out,sizeof(out),err);CHECK(n==-2 && !h.finished,"total trailer section bounded");
 setup(&h,normal,sizeof(normal)-1,-1,0,0);volatile int gen=2;n=http_read(&h,out,sizeof(out),0,&gen,1,err,sizeof(err));CHECK(n==-3 && pos==0,"cancellation before consuming bytes");
 setup(&h,(unsigned char*)"3\r\nabc\r\n0\r\n\r\nNEXT",17,-1,0,0);n=drain(&h,out,sizeof(out),err);CHECK(n==3 && !memcmp(out,"abc",3),"body stops at terminal trailer");CHECK(h.blen-h.bpos==4,"next protocol bytes retained in Http");http_close(&h);CHECK(!puts_count && closes_count==1,"unconsumed bytes prevent pool corruption");
 for(read_quantum=1;read_quantum<=20;read_quantum++){setup(&h,normal,sizeof(normal)-1,-1,1,0);n=drain(&h,out,sizeof(out),err);CHECK(n==16 && !memcmp(out,"0123456789abcdef",16),"small destination and bytewise timeouts");CHECK(h.body_bytes==16 && h.bytes_total==(int)sizeof(normal)-1,"production body and raw byte counters");}read_quantum=0;
 const unsigned char bws[]="1 \t; foo=bar\r\nx\r\n000\r\n\r\n";setup(&h,bws,sizeof(bws)-1,-1,1,0);n=drain(&h,out,sizeof(out),err);CHECK(n==1 && out[0]=='x',"extension BWS and leading zero last chunk");
 const unsigned char binary[]={ '3','\r','\n',0,'\r','\n','\r','\n','0','\r','\n','\r','\n'};setup(&h,binary,sizeof(binary),-1,1,0);n=drain(&h,out,sizeof(out),err);CHECK(n==3 && out[0]==0 && out[1]=='\r' && out[2]=='\n',"binary body bytes not parsed as framing");
 setup(&h,(unsigned char*)"7fffffff\r\n",10,-1,0,-1);n=http_read(&h,out,sizeof(out),0,NULL,-1,err,sizeof(err));CHECK(n==-1 && h.chunk_left==INT_MAX,"maximum supported chunk accepted without overflow");
 char trailers[1024];int tl=3;memcpy(trailers,"0\r\n",3);for(int i=0;i<16;i++){memcpy(trailers+tl,"X:a\r\n",5);tl+=5;}memcpy(trailers+tl,"\r\n",2);tl+=2;setup(&h,(unsigned char*)trailers,tl,-1,1,0);n=drain(&h,out,sizeof(out),err);CHECK(n==0 && h.finished && pos==tl,"more than eight valid trailers fully consumed");http_close(&h);CHECK(puts_count==1,"multi-trailer completion reusable");
 setup(&h,(unsigned char*)"1",1,-1,0,-1);unsigned before=clock_us;n=http_read(&h,out,sizeof(out),12,NULL,-1,err,sizeof(err));CHECK(n==-1 && !h.finished && clock_us-before>=12000 && clock_us-before<30000,"positive idle timeout budget bounded");
 setup(&h,(unsigned char*)"abc",3,-1,1,0);h.chunked=0;h.remaining=3;n=drain(&h,out,sizeof(out),err);CHECK(n==3 && !memcmp(out,"abc",3) && h.finished,"content length smoke unchanged");http_close(&h);CHECK(puts_count==1,"content length completion still reusable");
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''
f=out/'fixture.c';f.write_text(code,encoding='utf-8');exe=out/'fixture.exe'
r=subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-O0','-g','-Wall','-Wextra','-Werror',str(f),'-o',str(exe)])
if r.returncode:sys.exit(r.returncode)
sys.exit(subprocess.run([str(exe)]).returncode)
