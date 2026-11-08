"""Production rotated RGBA wrapper: CPU sample order and actual GE quad geometry."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/disc-rotation';O.mkdir(exist_ok=True);s=(R/'src/gfx.c').read_text(encoding='utf-8');a=s.index('void gfx_blit_rgba_rotated(');b=s.index('void gfx_blit_rgba_scaled(',a)
code=r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define FB_STRIDE 512
static unsigned short pixels[512*272];static int g_clip_x0,g_clip_y0,g_clip_x1=480,g_clip_y1=272,ge,calls;static float corners[8];
static int gu_ready(void){return ge;}static void gfx_cpu_barrier(void){}static unsigned short*fb(void){return pixels;}
static unsigned short gfx_rgb(int r,int g,int b){return(r>>3)|((g>>2)<<5)|((b>>3)<<11);}
static unsigned short blend(unsigned short d,unsigned short c,int a){return a>127?c:d;}
static int gfx_gu_quad8888(const unsigned char*p,int sw,int sh,const float xy[8],const float uv[8]){assert(p&&sw==2&&sh==2&&uv[6]==2&&uv[7]==2);memcpy(corners,xy,sizeof(corners));calls++;return 1;}
'''+s[a:b]+r'''
int main(void){unsigned char image[]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};gfx_blit_rgba_rotated(20,20,2,image,2,2,0);assert(pixels[20*512+20]==31&&pixels[20*512+21]==2016);memset(pixels,0,sizeof(pixels));gfx_blit_rgba_rotated(20,20,2,image,2,2,90);assert(pixels[20*512+20]==63488&&pixels[20*512+21]==31&&pixels[21*512+20]==65535);ge=1;gfx_blit_rgba_rotated(20,20,2,image,2,2,90);assert(calls==1&&fabsf(corners[0]-22)<.001f&&fabsf(corners[1]-20)<.001f&&fabsf(corners[2]-22)<.001f&&fabsf(corners[3]-22)<.001f);gfx_blit_rgba_rotated(20,20,300,image,2,2,0);assert(calls==1);puts("PASS rotated cover CPU clockwise sampling, GE corner/UV submission, size limit");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',O/'test.c','-lm','-o',exe],check=True);subprocess.run([exe],check=True)
