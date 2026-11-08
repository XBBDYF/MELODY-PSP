"""Actual gfx renderer state: switch snapshots, GU/CPU barriers, MV hold and init failure."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/ge-lifecycle';O.mkdir(exist_ok=True);s=(R/'src/gfx.c').read_text(encoding='utf-8');a=s.index('static int g_renderer,');b=s.index('static Font g_fonts',a)
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#define GFX_RENDERER_CPU 0
#define GFX_RENDERER_GE 1
#define FB_STRIDE 512
#define SCR_H 272
static unsigned char ram[2][512*272*2],vram[2][512*272*2];static unsigned char*g_ram[]={ram[0],ram[1]};static void*g_vram[]={vram[0],vram[1]};static unsigned short*g_fb[2];static int g_back,g_direct_video,g_ready=1;
static int init_result=1,begins,resumes,flushes,shutdowns;static unsigned ticks;
static int gfx_gu_init(void){return init_result;}static int gfx_gu_begin(void*p){assert((unsigned long long)p==0|| (unsigned long long)p==0x44000);begins++;return init_result;}
static void gfx_gu_resume(void){resumes++;}static void gfx_gu_flush(void){flushes++;}static void gfx_gu_shutdown(void){shutdowns++;}
static unsigned sceKernelGetSystemTimeLow(void){return ++ticks;}
#define mlog(...) ((void)0)
'''+s[a:b]+r'''
int main(void){ram[0][0]=17;ram[1][0]=19;g_fb[0]=(unsigned short*)ram[0];g_fb[1]=(unsigned short*)ram[1];assert(gfx_set_renderer(8)<0);assert(gfx_set_renderer(1)==0);assert(gu_ready()&&begins==1&&g_fb[0]==(unsigned short*)vram[0]&&vram[0][0]==17);gfx_cpu_barrier();assert(flushes==1);assert(gu_ready()&&resumes==1);vram[0][0]=88;assert(!gfx_set_renderer(0));assert(!gu_ready()&&ram[0][0]==88&&g_fb[0]==(unsigned short*)ram[0]);gfx_set_renderer(1);assert(gu_ready());int before=begins;gfx_suspend_renderer();assert(gfx_renderer()==1&&g_renderer==0&&!g_ge_frame);assert(!gu_ready()&&begins==before);gfx_resume_renderer();assert(gu_ready()&&begins==before+1);gfx_suspend_renderer();gfx_resume_renderer();init_result=0;assert(!gu_ready()&&gfx_renderer()==0&&gfx_renderer_error()[0]);assert(shutdowns>=3);puts("PASS actual renderer CPU/GE switch snapshots, flush/resume, MV no-reallocate hold, chosen preference resume, zero-return init failure CPU fallback");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-O2','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-misleading-indentation',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
