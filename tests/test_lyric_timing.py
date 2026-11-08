"""Compile actual lyric parser/motion code with a small host kernel-memory shim."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.tmp' / 'lyric-timing'
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'src/api.c').read_text(encoding='utf-8')
start = source.index('/* YRC timing is optional')
end = source.index('/* ------------------------------------------------------------------ */', start)
code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "app.h"
#include "lyric_motion.h"
#define PSP_SMEM_High 1
#define JSON_STR 1
#define mlog(...) ((void)0)
static Lyric g_lyric;
static void *block;
static int fail_alloc;
static int sceKernelAllocPartitionMemory(int a,const char*b,int c,int size,void*d) {
 (void)a;(void)b;(void)c;(void)d;
 if(fail_alloc)return -1;
 block=malloc((size_t)size);return block?1:-1;
}
static void *sceKernelGetBlockHeadAddr(int a){(void)a;return block;}
static void sceKernelFreePartitionMemory(int a){(void)a;free(block);block=NULL;}
''' + source[start:end] + r'''
int main(void) {
 int done,end;float f;
 parse_lyric("[00:01.25]original\n[00:02.000]next\n");
 assert(g_lyric.count==2 && g_lyric.ms[0]==1250);
 parse_translation("[00:01.260]must not shift ordinary LRC\n[00:02.000]next translation\n");
 assert(!g_lyric.translation[0][0] && !strcmp(g_lyric.translation[1],"next translation"));
 assert(!api_lyric_timing(0,1300,&done,&f) && done==0 && f==0);
 /* Real public endpoint fixture: song1901371647, yv=-1; absolute starts. */
 parse_yrc("[22310,4300](22310,2880,0)都 (25190,310,0)是(25500,290,0)勇(25790,230,0)敢(26020,590,0)的\n");
 assert(g_lyric.count==1 && g_lyric.ms[0]==22310);
 assert(!strcmp(g_lyric.text[0],"都 是勇敢的"));
 assert(api_lyric_word_span(0,23750,&done,&end,&f));
 assert(done==0 && end==4 && fabsf(f-.5f)<.0001f);
 assert(api_lyric_word_span(0,25345,&done,&end,&f));
 assert(done==4 && end==7 && fabsf(f-.5f)<.0001f);
 parse_translation("[00:22.290]translation\n");
 assert(g_lyric.translated_count==1 && !strcmp(g_lyric.translation[0],"translation"));
 parse_yrc("[10000,1000](10000,200,0)你好(10500,500,0) world\n[12000,1](12000,0,0)!\n");
 assert(api_lyric_word_span(0,10100,&done,&end,&f) && done==0 && end==6 && f==.5f);
 assert(api_lyric_word_span(0,10300,&done,&end,&f) && done==6 && end==6 && f==0);
 assert(api_lyric_word_span(0,9999,&done,&end,&f) && done==0 && end==0 && f==0);
 assert(api_lyric_word_span(0,11000,&done,&end,&f) && done==12 && end==12 && f==0);
 assert(api_lyric_timing(1,12000,&done,&f) && done==1 && f==0);
 assert(!api_lyric_timing(-1,0,&done,&f));
 /* Invalid/relative timestamps, overflow and metadata must preserve LRC. */
 parse_lyric("[00:20.000]fallback\n");
 parse_yrc("{metadata}\n[20000,1000](0,100,0)relative\n[999999999999999999999,1](0,0,0)x\n[20000,10](20000,11,0)bad\n");
 assert(g_lyric.count==1 && !strcmp(g_lyric.text[0],"fallback"));
 assert(!api_lyric_timing(0,20000,&done,&f));
 /* Capacity exhaustion never publishes partial-word timing for a line. */
 parse_yrc("[100,100](100,100,0)a\n");
 g_lyric_word_count=LYRIC_WORD_CAP;
 /* The full-song parser resets on first valid row, then bounds every append. */
 char many[40000];int n=0;
 for(int i=0;i<90;i++) n+=snprintf(many+n,sizeof(many)-(size_t)n,
 "[%d,10](%d,1,0)a(%d,1,0)b(%d,1,0)c(%d,1,0)d(%d,1,0)e(%d,1,0)f(%d,1,0)g(%d,1,0)h\n",
 i*10,i*10,i*10+1,i*10+2,i*10+3,i*10+4,i*10+5,i*10+6,i*10+7);
 parse_yrc(many);
 assert(g_lyric.count==90 && g_lyric_word_count==640);
 assert(!api_lyric_timing(89,895,&done,&f));
 lyric_timing_free(); assert(!block && !g_lyric_words && !g_lyric_word_count);
 parse_lyric("[00:01.000]fallback\n"); fail_alloc=1;
 parse_yrc("[1000,100](1000,100,0)x\n");
 assert(!strcmp(g_lyric.text[0],"fallback") && !g_lyric_words);
 /* Callback paths, including translation, compile and execute from source. */
 fail_alloc=0;yrc_cb(NULL,"yrc.lyric",JSON_STR,"[1000,100](1000,100,0)x",0);
 translation_cb(NULL,"tlyric.lyric",JSON_STR,"[00:01.000]translated",0);
 assert(!strcmp(api_lyric()->translation[0],"translated"));
 lyric_timing_free();
 float a=lyric_scroll_position(0,1,.016f,100);
 float b=lyric_scroll_position(0,1,.016f,2000);
 assert(a>b && a<1 && b>0);
 assert(lyric_scroll_position(0,20,.016f,100)==20);
 assert(lyric_motion_step(0,1,0,.1f)==0);
 assert(lyric_motion_step(0,1,1,.1f)<=1);
 assert(isfinite(lyric_motion_step(NAN,1,.1f,.1f)));
 assert(isfinite(lyric_motion_step(1,NAN,.1f,.1f)));
 float x=0,y=0;
 for(int i=0;i<60;i++)x=lyric_motion_step(x,1,1.f/60,.2f);
 for(int i=0;i<30;i++)y=lyric_motion_step(y,1,1.f/30,.2f);
 assert(fabsf(x-y)<.00001f);
 puts("PASS real absolute YRC, UTF-8 spans, gaps, zero duration, fallback, translation, bounds, MV release, adaptive motion");
 return 0;
}
'''
(OUT / 'test.c').write_text(code, encoding='utf-8')
compiler = shutil.which('gcc') or 'D:/msys64/ucrt64/bin/gcc.exe'
exe = OUT / 'test.exe'
subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-I', str(ROOT / 'src'),
                str(OUT / 'test.c'), str(ROOT / 'src/lyric_motion.c'), '-lm', '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
assert '&tv=-1&yv=-1' in source
assert source.count('json_walk(lbuf,(int)strlen(lbuf),yrc_cb,NULL)') == 1
assert source.count('json_walk(g_buf,(int)strlen(g_buf),yrc_cb,NULL)') == 1
assert 'lyric_timing_free();' in source[source.index('void api_free_buffers(void)'):]
print('PASS live/cache YRC passes and requested endpoint flag')
