/* Identical deterministic color work, without decoder/pacer/audio waits. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef __PSP__
#include <pspkernel.h>
#include <psppower.h>
PSP_MODULE_INFO("MV565 Color Bench", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(2048);
static uint64_t now_us(void){return sceKernelGetSystemTimeWide();}
#else
#include <time.h>
static uint64_t now_us(void){return (uint64_t)clock()*1000000/CLOCKS_PER_SEC;}
#endif
#include "bgr565_render.h"
#ifndef PSP_BGR565_LUT
#define PSP_BGR565_LUT 0
#endif
static YuvRgbTables tables;
static uint32_t output[512*272];
static uint8_t yp[432*240],up[216*120],vp[216*120];
static unsigned times[240];
static volatile uint32_t check;
static int cmp(const void *a,const void *b){unsigned x=*(const unsigned*)a,y=*(const unsigned*)b;return x>y?1:x<y?-1:0;}
int main(void){
#ifdef __PSP__
 scePowerSetClockFrequency(333,333,166);
#endif
 const uint8_t *planes[]={yp,up,vp};int stride[]={432,216,216};
 uint32_t rng=12345;uint64_t sum=0;
 for(unsigned i=0;i<sizeof(yp);i++){rng=rng*1664525u+1013904223u;yp[i]=(uint8_t)(rng>>24);}
 for(unsigned i=0;i<sizeof(up);i++){rng=rng*1664525u+1013904223u;up[i]=(uint8_t)(rng>>24);rng=rng*1664525u+1013904223u;vp[i]=(uint8_t)(rng>>24);}
 yuv_rgb_init(&tables);
 for(int i=0;i<250;i++){
  yp[i%sizeof(yp)]^=(uint8_t)i;
  uint64_t begin=now_us();
  bgr565_render_native(planes,stride,426,240,output,&tables);
  unsigned elapsed=(unsigned)(now_us()-begin);
  check^=output[16*512+20+i%100];
  if(i>=10){times[i-10]=elapsed;sum+=elapsed;}
 }
 qsort(times,240,sizeof(times[0]),cmp);
#ifdef __PSP__
 FILE *f=fopen("ms0:/PSP/GAME/MVTest/colorbench.txt","w");
#else
 FILE *f=stdout;
#endif
 if(f){fprintf(f,"lut=%d samples=240 mean_us=%llu p50_us=%u p95_us=%u p99_us=%u check=%u table_bytes=%u\n",PSP_BGR565_LUT,(unsigned long long)(sum/240),times[120],times[228],times[237],(unsigned)check,(unsigned)sizeof(tables));
#ifdef __PSP__
 fclose(f);
#endif
 }
#ifdef __PSP__
 sceKernelExitGame();
#endif
 return 0;
}
