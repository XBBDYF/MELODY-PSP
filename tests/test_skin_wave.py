"""Compile production broad-wave geometry against a recorded renderer/kernel."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/skin-wave';O.mkdir(exist_ok=True)
(O/'pspkernel.h').write_text('typedef int SceUID;\n');(O/'pspsysmem.h').write_text('#define PSP_SMEM_High 1\nint sceKernelAllocPartitionMemory(int,const char*,int,unsigned,void*);\nvoid*sceKernelGetBlockHeadAddr(int);\nint sceKernelFreePartitionMemory(int);\n')
code=r'''
#include <assert.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "skin_wave.h"
static void*memory;static int fail,allocs,frees,quads;static float first;static unsigned alpha_peak;static unsigned char red;
int sceKernelAllocPartitionMemory(int p,const char*n,int kind,unsigned size,void*a){(void)n;(void)a;assert(p==2&&kind==1&&size==4096);if(fail)return -1;memory=malloc(size);allocs++;return 5;}
void*sceKernelGetBlockHeadAddr(int id){assert(id==5);return memory;}int sceKernelFreePartitionMemory(int id){assert(id==5);free(memory);memory=0;frees++;return 0;}
void gfx_blit_rgba_quad(const unsigned char*p,int sw,int sh,const float xy[8],const float uv[8],int a){assert(sw==8&&sh==128&&p==memory&&a>0&&a<=255&&uv[2]==8&&uv[7]==128);for(int i=0;i<8;i++)assert(isfinite(xy[i]));assert(xy[0]>=0&&xy[2]<=480&&xy[2]-xy[0]==16&&xy[5]-xy[1]>35&&xy[7]-xy[3]>35);if(!quads)first=xy[1];quads++;for(int y=0;y<128;y++)if(p[y*8*4+3]>alpha_peak)alpha_peak=p[y*8*4+3];red=p[(64*8)*4];}
int main(void){fail=1;skin_wave_draw(0,31);assert(!quads&&!allocs);fail=0;skin_wave_draw(0,31);assert(quads==90&&allocs==1&&alpha_peak>100);float old=first;unsigned char r=red;quads=0;skin_wave_draw(20,31);assert(quads==90&&allocs==1&&fabsf(first-old)>1);quads=0;skin_wave_draw(20,63488);assert(quads==90&&allocs==1&&red!=r);quads=0;skin_wave_draw(NAN,63488);assert(quads==90);skin_wave_release();skin_wave_release();assert(frees==1&&!memory);puts("PASS production three broad B-spline layers, highlight alpha, changing geometry,4KiB reuse/release and allocation failure");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-O2','-Wall','-Wextra','-Werror','-I'+str(O),'-Isrc',O/'test.c',R/'src/skin_wave.c','-lm','-o',exe],cwd=R,check=True);subprocess.run([exe],check=True)
