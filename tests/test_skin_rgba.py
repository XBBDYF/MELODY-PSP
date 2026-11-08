from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/skin-rgba';O.mkdir(exist_ok=True);s=(R/'src/gfx.c').read_text(encoding='utf-8');a=s.index('void gfx_blit_rgba_scaled(');b=s.index('/* 鏁村潡',a)if'/* 鏁村潡'in s[a:]else s.index('static inline int rect_inside(',a)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define FB_STRIDE 512
#define GFX_GU_8888 3
#define GFX_GU_5650 0
static int gu_ready(void){return 0;}
static void gfx_cpu_barrier(void){}
static int gfx_gu_image(int x,int y,int w,int h,const void*p,int sw,int sh,int stride,int format,int a){(void)x;(void)y;(void)w;(void)h;(void)p;(void)sw;(void)sh;(void)stride;(void)format;(void)a;return 0;}
static int gfx_gu_quad565(const unsigned short*p,int sw,int sh,int stride,const float xy[8],const float uv[8],unsigned short t){(void)p;(void)sw;(void)sh;(void)stride;(void)xy;(void)uv;(void)t;return 0;}
static unsigned short pixels[512*272];static int g_clip_x0=0,g_clip_y0=0,g_clip_x1=480,g_clip_y1=272;
static unsigned short*fb(void){return pixels;}
static unsigned short gfx_rgb(int r,int g,int b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static unsigned short blend(unsigned short d,unsigned short s,int a){return a==255?s:(unsigned short)((s*a+d*(255-a))/255);}
'''+s[a:b]+r'''
int main(void){unsigned char image[]={255,0,0,255,0,255,0,0,0,0,255,128,255,255,255,255};gfx_blit_rgba_scaled(2,3,4,4,image,2,2,255);assert(pixels[3*512+2]==gfx_rgb(255,0,0));assert(pixels[3*512+5]==0);assert(pixels[6*512+5]==65535);assert(pixels[6*512+2]!=0&&pixels[6*512+2]!=gfx_rgb(0,0,255));g_clip_x0=3;g_clip_y0=4;g_clip_x1=5;g_clip_y1=6;unsigned short before=pixels[3*512+2];gfx_blit_rgba_scaled(0,0,10,10,image,2,2,128);assert(pixels[3*512+2]==before);gfx_blit_rgba_scaled(0,0,-1,5,image,2,2,255);unsigned short source[]={1,2,3,4};g_clip_x0=g_clip_y0=0;g_clip_x1=480;g_clip_y1=272;gfx_blit565_scaled(10,10,4,4,source,2,2);assert(pixels[10*512+10]==1&&pixels[13*512+13]==4);puts("PASS production RGBA alpha/global alpha/scaling/clip and album565 scaling");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
