"""Exercise the real native lyric renderer with deterministic UTF-8 glyph widths."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.tmp' / 'native-lyric-motion'
OUT.mkdir(parents=True, exist_ok=True)
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "api.h"
#include "gfx.h"
#include "lyric_render.h"
#include "lyric_motion.h"
static Lyric lyric;
static int timed=1, done, end;
static float fraction;
static int cx0,cy0,cx1=480,cy1=272,depth,fail_at=-1;
static int saved[8][4], draws, overlay_width, overlay_x, overlay_y, overlay_bottom;
static unsigned short colors[8];
const Lyric *api_lyric(void){return &lyric;}
int api_lyric_word_span(int i,int p,int *d,int *e,float *f){
 (void)p;if(!timed||i<0||i>=lyric.count)return 0;*d=done;*e=end;*f=fraction;return 1;
}
int gfx_font_h(int f){(void)f;return 16;}
int gfx_text_w_n(int f,const char *s,int n){
 (void)f;int w=0,i=0;while(s[i]&&i<n){unsigned char c=(unsigned char)s[i];
 int len=c<128?1:(c<224?2:(c<240?3:4));if(i+len>n)break;
 w+=len==1?8:16;i+=len;}return w;
}
int gfx_text_w(int f,const char*s){return gfx_text_w_n(f,s,(int)strlen(s));}
int gfx_push_clip(int x0,int y0,int x1,int y1){
 if(depth==fail_at)return -1;
 saved[depth][0]=cx0;saved[depth][1]=cy0;saved[depth][2]=cx1;saved[depth][3]=cy1;++depth;
 if(x0>cx0)cx0=x0;if(y0>cy0)cy0=y0;if(x1<cx1)cx1=x1;if(y1<cy1)cy1=y1;return 0;
}
void gfx_pop_clip(void){assert(depth>0);--depth;cx0=saved[depth][0];cy0=saved[depth][1];cx1=saved[depth][2];cy1=saved[depth][3];}
int gfx_text(int f,int x,int y,const char*s,unsigned short c){
 (void)f;(void)x;(void)y;(void)s;colors[draws++]=c;
 if(c==2){overlay_width=cx1-cx0;overlay_x=cx0;overlay_y=cy0;overlay_bottom=cy1;}return x;
}
int gfx_text_clip(int f,int x,int y,int w,const char*s,unsigned short c){(void)w;return gfx_text(f,x,y,s,c);}
static void reset(void){draws=0;overlay_width=-1;fail_at=-1;assert(depth==1);}
static void draw(const char *s,int offset,int w){lyric_draw_text(10,45,w,s,FS_MD,1,2,0,1000,offset);assert(depth==1);assert(cx0==0&&cy0==48&&cx1==480&&cy1==226);}
int main(void){
 lyric.count=1;strcpy(lyric.text[0],"A你好BC");
 assert(gfx_push_clip(0,48,480,226)==0);
 /* Half of one Chinese glyph is eight pixels, never half its UTF-8 bytes. */
 done=1;end=4;fraction=.5f;
 assert(lyric_highlight_width(FS_MD,lyric.text[0],0,1000)==16);
 reset();draw(lyric.text[0],0,100);assert(draws==2&&colors[0]==1&&colors[1]==2);
 assert(overlay_x==10&&overlay_width==16&&overlay_y==48&&overlay_bottom==61);
 /* One word spans a wrap boundary: global width minus fragment prefix. */
 done=1;end=7;fraction=.75f;
 reset();draw("好BC",4,100);assert(overlay_width==8);
 reset();draw("A你",0,100);assert(overlay_width==24);
 /* Completed prefix after this fragment, before fragment, and explicit width. */
 done=end=9;fraction=0;reset();draw("好BC",4,10);assert(overlay_width==10);
 done=end=1;reset();draw("好BC",4,100);assert(draws==1&&overlay_width==-1);
 timed=0;reset();draw(lyric.text[0],0,100);assert(draws==1&&colors[0]==2);
 assert(lyric_highlight_width(FS_MD,lyric.text[0],0,1000)==-1);timed=1;
 /* Exhausted scopes leave the base visible and never pop an outer clip. */
 done=1;end=4;fraction=.5f;reset();fail_at=1;draw(lyric.text[0],0,100);
 assert(draws==1&&colors[0]==1);
 done=-100;end=10000;fraction=2;assert(lyric_highlight_width(FS_MD,lyric.text[0],0,1000)==56);
 float quick=lyric_scroll_position(0,1,.016f,100),slow=lyric_scroll_position(0,1,.016f,3000);
 assert(quick>slow&&quick<1&&slow>0);
 float up=lyric_scroll_position(2,1,.016f,3000);assert(up>1&&up<2);
 assert(lyric_scroll_position(0,1,0,3000)==0);
 gfx_pop_clip();puts("PASS native lyric UTF-8 pixel overlay, wrapped spans, clip preservation/failure, fallback, adaptive up/down motion");
 return 0;
}
'''
(OUT / 'test.c').write_text(code, encoding='utf-8')
compiler = shutil.which('gcc') or 'D:/msys64/ucrt64/bin/gcc.exe'
exe = OUT / 'test.exe'
subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-I', str(ROOT / 'src'),
                str(OUT / 'test.c'), str(ROOT / 'src/lyric_render.c'),
                str(ROOT / 'src/lyric_motion.c'), '-lm', '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
ui = (ROOT / 'src/ui.c').read_text(encoding='utf-8')
screen = ui[ui.index('static void screen_lyrics('):ui.index('static const char *g_kb0')]
assert 'lyric_scroll_position(g_app.lyric_scroll / line_h,' in screen
assert 'dt, gap_ms) * line_h' in screen
assert 'if (cur < 0) g_app.lyric_scroll = 0;' in screen
assert 'ly->ms[cur + 1] - ly->ms[cur] : 3000' in screen
assert 'fabsf(g_app.lyric_scroll - target)' not in screen
assert 'lyric_draw_text(tx, y, tw' in screen
assert 'gfx_fill_round_a(tx - 10' not in screen
print('PASS native page uses shared line motion and real YRC glyph rendering')
