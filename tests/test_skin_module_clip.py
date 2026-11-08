from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/skin-clip';O.mkdir(exist_ok=True);s=(R/'src/gfx.c').read_text(encoding='utf-8');a=s.index('static int g_clip_x0');b=s.index('static unsigned short *fb',a);c=s.index('static void set_clip(',b);d=s.index('static inline int clip_rect(',c)
code='#include <assert.h>\n#include <stdio.h>\n#define SCR_W 480\n#define SCR_H 272\n#define GFX_RENDERER_GE 1\nstatic int g_renderer,g_ge_frame;\nstatic void gfx_gu_clip(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}\n'+s[a:b]+s[c:d]+r'''
int main(void){gfx_reset_clip();assert(!gfx_push_clip(50,40,240,150));gfx_reset_clip();assert(g_clip_x0==50&&g_clip_y0==40&&g_clip_x1==240&&g_clip_y1==150);gfx_set_clip(0,0,480,272);assert(g_clip_x0==50&&g_clip_x1==240);assert(!gfx_push_clip(70,60,130,100));gfx_reset_clip();assert(g_clip_x0==70&&g_clip_x1==130);gfx_pop_clip();assert(g_clip_x0==50&&g_clip_x1==240);gfx_pop_clip();assert(g_clip_x0==0&&g_clip_y0==0&&g_clip_x1==480&&g_clip_y1==272);puts("PASS native module reset/set cannot escape scoped viewport; nested scope restores caller clip");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
