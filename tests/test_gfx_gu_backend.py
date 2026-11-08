"""Compile the full GU module against a recording PSP SDK shim."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

SHIM = r'''
#include <stddef.h>
#include <stdint.h>
#define GU_DIRECT 0
#define GU_PSM_5650 0
#define GU_PSM_8888 3
#define GU_SCISSOR_TEST 1
#define GU_DEPTH_TEST 2
#define GU_CULL_FACE 3
#define GU_LIGHTING 4
#define GU_BLEND 5
#define GU_TEXTURE_2D 6
#define GU_ADD 0
#define GU_SRC_ALPHA 1
#define GU_ONE_MINUS_SRC_ALPHA 2
#define GU_TFX_MODULATE 0
#define GU_TCC_RGBA 1
#define GU_NEAREST 0
#define GU_CLAMP 0
#define GU_SPRITES 6
#define GU_TRIANGLE_STRIP 4
#define GU_TEXTURE_32BITF 1
#define GU_VERTEX_32BITF 2
#define GU_TRANSFORM_2D 4
#define GU_COLOR_8888 8
#define PSP_SMEM_High 1
typedef int SceUID;
SceUID sceKernelAllocPartitionMemory(int,const char*,int,unsigned int,void*);
void *sceKernelGetBlockHeadAddr(SceUID);
int sceKernelFreePartitionMemory(SceUID);
void sceKernelDcacheWritebackRange(const void*,unsigned int);
void sceKernelDcacheWritebackInvalidateRange(const void*,unsigned int);
void sceKernelDcacheInvalidateRange(const void*,unsigned int);
void sceGuCopyImage(int,int,int,int,int,int,const void*,int,int,int,void*);
void sceGuInit(void); void sceGuTerm(void);
void sceGuStart(int,void*); int sceGuFinish(void); int sceGuSync(int,int);
void sceGuDrawBuffer(int,void*,int); void sceGuOffset(int,int);
void sceGuViewport(int,int,int,int); void sceGuScissor(int,int,int,int);
void sceGuEnable(int); void sceGuDisable(int);
void sceGuBlendFunc(int,int,int,unsigned int,unsigned int);
void sceGuColor(unsigned int); void *sceGuGetMemory(int);
void sceGuDrawArray(int,int,int,const void*,const void*);
void sceGuTexMode(int,int,int,int); void sceGuTexImage(int,int,int,int,const void*);
void sceGuTexFunc(int,int); void sceGuTexFilter(int,int);
void sceGuTexWrap(int,int); void sceGuTexFlush(void);
'''

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_gu.h"
#include "pspgu.h"
static unsigned char pool[768*1024] __attribute__((aligned(64)));
static unsigned char vertices[4096] __attribute__((aligned(16)));
static int fail_alloc, allocated, frees, syncs, draws, textures, active;
static int clip[4], tw,th,ts,tf; static unsigned color;
static const unsigned char *tex; static void *drawbuf;
static float lastuv[4],lastxy[4];
SceUID sceKernelAllocPartitionMemory(int p,const char*n,int t,unsigned int s,void*a){(void)n;(void)a;assert(p==2&&t==PSP_SMEM_High&&s<=sizeof(pool));if(fail_alloc)return -1;allocated++;return 1;}
void *sceKernelGetBlockHeadAddr(SceUID i){(void)i;return pool;}
int sceKernelFreePartitionMemory(SceUID i){(void)i;frees++;return 0;}
void sceKernelDcacheWritebackRange(const void*p,unsigned int n){(void)p;(void)n;}
void sceKernelDcacheWritebackInvalidateRange(const void*p,unsigned n){(void)p;(void)n;}
void sceKernelDcacheInvalidateRange(const void*p,unsigned n){(void)p;(void)n;}
void sceGuCopyImage(int f,int sx,int sy,int w,int h,int ss,const void*src,int dx,int dy,int ds,void*dst){assert(f==0&&sx==0&&sy==0&&w==480&&h==272&&ss==512&&dx==0&&dy==0&&ds==512&&src&&dst);memcpy(dst,src,512*272*2);}
void sceGuInit(void){} void sceGuTerm(void){}
void sceGuStart(int t,void*p){(void)t;(void)p;assert(!active);active=1;}
int sceGuFinish(void){assert(active);active=0;return 0;}
int sceGuSync(int a,int b){(void)a;(void)b;assert(!active);syncs++;return 0;}
void sceGuDrawBuffer(int f,void*p,int s){assert(f==0&&s==512);drawbuf=p;}
void sceGuOffset(int a,int b){(void)a;(void)b;}
void sceGuViewport(int a,int b,int c,int d){(void)a;(void)b;assert(c==480&&d==272);}
void sceGuScissor(int a,int b,int c,int d){clip[0]=a;clip[1]=b;clip[2]=c;clip[3]=d;}
void sceGuEnable(int a){(void)a;} void sceGuDisable(int a){(void)a;}
void sceGuBlendFunc(int a,int b,int c,unsigned int d,unsigned int e){(void)a;(void)b;(void)c;(void)d;(void)e;}
void sceGuColor(unsigned int c){color=c;}
void *sceGuGetMemory(int n){assert(n<4096);return vertices;}
void sceGuDrawArray(int p,int f,int n,const void*i,const void*v){(void)i;assert(active&&((p==GU_SPRITES&&n==2)||(p==GU_TRIANGLE_STRIP&&n==4)));draws++;if(f&GU_TEXTURE_32BITF){const float*q=v;lastuv[0]=q[0];lastuv[1]=q[1];lastuv[2]=q[5];lastuv[3]=q[6];lastxy[0]=q[2];lastxy[1]=q[3];lastxy[2]=q[7];lastxy[3]=q[8];}}
void sceGuTexMode(int a,int b,int c,int d){tf=a;(void)b;(void)c;(void)d;}
void sceGuTexImage(int l,int w,int h,int s,const void*p){(void)l;tw=w;th=h;ts=s;tex=p;textures++;}
void sceGuTexFunc(int a,int b){assert(a==GU_TFX_MODULATE&&b==GU_TCC_RGBA);}
void sceGuTexFilter(int a,int b){(void)a;(void)b;}
void sceGuTexWrap(int a,int b){(void)a;(void)b;}
void sceGuTexFlush(void){}
int main(void){
 GfxGuStats stats; unsigned short src[10]={1,2,3,99,99,4,5,6,99,99}; unsigned char rgba[8]={1,2,3,17,4,5,6,255}; unsigned char bits[12]={0x80,0x40};
 fail_alloc=1;assert(!gfx_gu_init());assert(!allocated);fail_alloc=0;
 assert(gfx_gu_init());assert(allocated==1);assert(gfx_gu_begin((void*)0x44000));assert(drawbuf==(void*)0x44000);
 gfx_gu_clip(-4,7,700,290);assert(clip[0]==0&&clip[1]==7&&clip[2]==480&&clip[3]==272);gfx_gu_clip(0,0,480,272);
 gfx_gu_rect(1,2,20,30,31,128);assert(draws==1);assert((color&0xffffff)==255&&color>>24==128);
 assert(gfx_gu_image(10,20,30,20,src,3,2,5,0,255));assert(tf==0&&tw==4&&th==2&&ts==4);assert(tex!=(void*)src);assert(((unsigned short*)tex)[0]==1&&((unsigned short*)tex)[3]==0&&((unsigned short*)tex)[4]==4);src[0]=42;assert(((unsigned short*)tex)[0]==1);assert(lastuv[2]==3&&lastuv[3]==2);
 assert(gfx_gu_image(0,0,2,1,rgba,2,1,2,3,128));assert(tf==3&&tex[3]==17&&tex[7]==255&&color>>24==128);
 assert(gfx_gu_glyph(0,0,2,2,bits,31,255));assert(tf==3);assert(((unsigned*)tex)[0]==0xff0000ff&&((unsigned*)tex)[1]==0&&((unsigned*)tex)[2]==0&&((unsigned*)tex)[3]==0xff0000ff);
 {unsigned before;gfx_gu_get_stats(&stats);before=stats.uploads;assert(gfx_gu_glyph(5,5,2,2,bits,31,128));gfx_gu_get_stats(&stats);assert(stats.uploads==before&&color>>24==128);}
 {int before=draws;assert(!gfx_gu_image(0,0,10,10,src,513,1,513,0,255));assert(!gfx_gu_image(0,0,10,10,src,3,2,2,0,255));assert(draws==before);}
 {float xy[8]={10,20,40,20,10,40,40,40},uv[8]={0,0,3,0,0,2,3,2};unsigned before;gfx_gu_get_stats(&stats);before=stats.uploads;assert(gfx_gu_quad565(src,3,2,5,xy,uv,65535));gfx_gu_get_stats(&stats);assert(stats.uploads==before);assert(((unsigned short*)tex)[0]==1);assert(gfx_gu_quad565(src,3,2,5,xy,uv,65535));gfx_gu_get_stats(&stats);assert(stats.uploads==before);assert(lastxy[0]==10&&lastxy[1]==20&&lastuv[2]==3);}
 gfx_gu_flush();assert(!active);gfx_gu_resume();assert(active&&drawbuf==(void*)0x44000&&clip[1]==0);
 assert(gfx_gu_image(0,0,3,2,src,3,2,5,0,255));assert(((unsigned short*)tex)[0]==42);
 {int i;for(i=0;i<4000;i++)gfx_gu_rect(0,0,1,1,0,255);assert(syncs>1);}
 {unsigned *large=calloc(512*512,4);assert(large);assert(gfx_gu_image(0,0,480,272,large,512,512,512,3,255));free(large);assert(syncs>2);}
 gfx_gu_clip(0,76,480,236);{int before=draws;assert(gfx_gu_image(0,-100,20,20,rgba,2,1,2,3,255));assert(draws==before);assert(gfx_gu_image(0,60,20,40,rgba,2,1,2,3,255));assert(lastxy[1]==76&&lastxy[3]<=236&&lastuv[1]>.39f);assert(gfx_gu_glyph(0,70,8,12,bits,31,255));assert(lastxy[1]==76&&lastuv[1]==6);}
 gfx_gu_end();assert(!active);gfx_gu_get_stats(&stats);assert(stats.draws==(unsigned)draws&&stats.uploads>=5&&stats.flushes==(unsigned)syncs);
 {unsigned short*src=calloc(512*272,2),*dst=calloc(512*272,2);src[0]=1234;src[512*271+479]=5678;assert(gfx_gu_readback565(src,dst));assert(dst[0]==1234&&dst[512*271+479]==5678);free(src);free(dst);}
 gfx_gu_shutdown();assert(frees==1);gfx_gu_shutdown();assert(frees==1);return 0;
}
'''

class GuBackendTests(unittest.TestCase):
    def test_real_module_against_recording_sdk(self):
        self.assertTrue((ROOT / 'src/gfx_gu.c').is_file(), 'GU backend implementation missing')
        cc = shutil.which('gcc')
        self.assertIsNotNone(cc, 'host C compiler required')
        with tempfile.TemporaryDirectory(prefix='melody_gu_') as temp:
            tmp = pathlib.Path(temp)
            for name in ('pspgu.h', 'pspkernel.h', 'pspsysmem.h'):
                (tmp / name).write_text('#pragma once\n' + SHIM if name == 'pspgu.h' else '#include "pspgu.h"\n')
            (tmp / 'harness.c').write_text(HARNESS)
            exe = tmp / 'gu_test.exe'
            result = subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-I'+str(tmp), '-I'+str(ROOT/'src'), str(ROOT/'src/gfx_gu.c'), str(tmp/'harness.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
