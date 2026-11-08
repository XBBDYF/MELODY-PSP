/* Independent broad-wave reconstruction: cubic control curves and soft env-like
 * highlight texture. This is not Sony firmware code or copied model data. */
#include "skin_wave.h"
#include "gfx.h"
#include <pspkernel.h>
#include <pspsysmem.h>
#include <math.h>
#define WAVE_W 8
#define WAVE_H 128
#define WAVE_BYTES (WAVE_W*WAVE_H*4)
static SceUID wave_block=-1;
static unsigned char*wave_pixels;
static unsigned short wave_color;
static int colored;
void skin_wave_release(void){wave_pixels=0;colored=0;if(wave_block>=0)sceKernelFreePartitionMemory(wave_block);wave_block=-1;}
static int prepare(unsigned short color){
 if(!wave_pixels){wave_block=sceKernelAllocPartitionMemory(2,"skin_wave",PSP_SMEM_High,WAVE_BYTES,0);if(wave_block<0)return 0;wave_pixels=sceKernelGetBlockHeadAddr(wave_block);if(!wave_pixels){skin_wave_release();return 0;}}
 if(colored&&wave_color==color)return 1;
 wave_color=color;colored=1;
 int r=(color&31)*255/31,g=((color>>5)&63)*255/63,b=((color>>11)&31)*255/31;
 for(int y=0;y<WAVE_H;y++){
  float v=y/(float)(WAVE_H-1),distance=(v-.47f)/.34f;
  float body=expf(-distance*distance*2.f),edge=expf(-powf((v-.20f)/.045f,2.f));
  float shine=.32f+.40f*body+.28f*edge;
  int a=(int)(185.f*body+65.f*edge);if(a>250)a=250;
  for(int x=0;x<WAVE_W;x++){unsigned char*p=wave_pixels+(y*WAVE_W+x)*4;p[0]=(unsigned char)(r*shine+45.f*edge);p[1]=(unsigned char)(g*shine+45.f*edge);p[2]=(unsigned char)(b*shine+35.f*edge);p[3]=(unsigned char)a;}
 }
 return 1;
}
/* Uniform cubic B-spline evaluation with independently moving control curves. */
static float spline(const float*c,float u){int i=(int)floorf(u);float t=u-i,tt=t*t,ttt=tt*t;return ((1-3*t+3*tt-ttt)*c[i]+(4-6*tt+3*ttt)*c[i+1]+(1+3*t+3*tt-3*ttt)*c[i+2]+ttt*c[i+3])/6.f;}
static void curves(int layer,float time,float*center,float*thick){
 float phase=time*.11f+layer*.9f;
 for(int i=0;i<11;i++){
  float x=(i-1)*.79f;
  /* Slow amplitude/frequency modulation and two phase targets keep width,
   * center and highlight from moving as a rigid single sine line. */
  float morph=.5f+.5f*sinf(time*.041f+layer*.7f);
  float a=33.f*sinf(x+phase)+15.f*cosf(x*.57f-phase*.63f);
  float b=27.f*cosf(x*.83f-phase*.8f)+18.f*sinf(x*.44f+phase*.72f);
  center[i]=145.f+layer*16.f+a*(1-morph)+b*morph;
  thick[i]=54.f+layer*9.f+17.f*sinf(x*.66f+phase*.31f+layer);
 }
}
void skin_wave_draw(float time,unsigned short accent){
 if(!isfinite(time))time=0;
 time=fmodf(time,100000.f);
 if(!prepare(accent))return;
 const int opacity[3]={104,78,58};
 for(int layer=2;layer>=0;--layer){float center[11],thick[11];curves(layer,time,center,thick);
  for(int i=0;i<30;i++){float x0=i*16.f,x1=(i+1)*16.f,u0=i*7.f/30.f,u1=(i+1)*7.f/30.f,c0=spline(center,u0),c1=spline(center,u1),w0=spline(thick,u0),w1=spline(thick,u1);
   float xy[8]={x0,c0-w0*.64f,x1,c1-w1*.64f,x0,c0+w0*.82f,x1,c1+w1*.82f};
   const float uv[8]={0,0,WAVE_W,0,0,WAVE_H,WAVE_W,WAVE_H};
   gfx_blit_rgba_quad(wave_pixels,WAVE_W,WAVE_H,xy,uv,opacity[layer]);
  }
 }
}
