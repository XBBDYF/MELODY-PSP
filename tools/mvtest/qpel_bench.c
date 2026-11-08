#include <pspkernel.h>
#include <psppower.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "h264qpel.h"
#include "exact_qpel.h"
PSP_MODULE_INFO("Exact Qpel Bench",0,1,0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(1024);
static uint8_t src[64*32] __attribute__((aligned(64)));
static uint8_t refbuf[64*32] __attribute__((aligned(64)));
static uint8_t optbuf[64*32] __attribute__((aligned(64)));
static volatile unsigned checksum;
int main(void){
 scePowerSetClockFrequency(333,333,166);
 H264QpelContext ref={0},opt={0};ff_h264qpel_init(&ref,8);opt=ref;exact_qpel_init(&opt);
 uint32_t rng=12345;for(unsigned i=0;i<sizeof(src);i++){rng=rng*1664525u+1013904223u;src[i]=rng>>24;}
 FILE *f=fopen("ms0:/PSP/GAME/MVTest/qpelbench.txt","w");if(!f){sceKernelExitGame();return 1;}
 for(int dim=0;dim<3;dim++)for(int phase=1;phase<=3;phase+=2)for(int avg=0;avg<2;avg++){
  uint64_t elapsed[2];
  for(int mode=0;mode<2;mode++){
   uint8_t *dst=mode?optbuf:refbuf;memset(dst,93,sizeof(refbuf));
   H264QpelContext *c=mode?&opt:&ref;
   qpel_mc_func fn=avg?c->avg_h264_qpel_pixels_tab[dim][phase]:c->put_h264_qpel_pixels_tab[dim][phase];
   uint64_t start=sceKernelGetSystemTimeWide();
   for(int i=0;i<5000;i++){fn(dst+64*4,src+64*4+7,64);checksum^=dst[64*4+(i&3)];}
   elapsed[mode]=sceKernelGetSystemTimeWide()-start;
  }
  int same=!memcmp(refbuf,optbuf,sizeof(refbuf));
  fprintf(f,"size=%d phase=%d avg=%d reference_us=%llu fused_us=%llu same=%d\n",16>>dim,phase,avg,(unsigned long long)elapsed[0],(unsigned long long)elapsed[1],same);fflush(f);
 }
 fclose(f);sceKernelExitGame();return 0;
}
