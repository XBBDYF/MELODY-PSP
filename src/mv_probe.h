#ifndef MELODY_MV_PROBE_H
#define MELODY_MV_PROBE_H
#include <stdint.h>
#include <stddef.h>
typedef struct{int profile,level,width,height;}MvProbe;
static inline uint32_t mv_be32(const unsigned char*p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
/* Inspect bounded MP4 sample-entry/avcC headers; unknown formats stay software. */
static inline void mv_probe_bytes(MvProbe*v,const unsigned char*p,size_t n){
 for(size_t i=4;i+12<=n;++i){
  uint32_t z=mv_be32(p+i-4);if(z<12||z>n-(i-4))continue;
  if(p[i]=='a'&&p[i+1]=='v'&&p[i+2]=='c'&&p[i+3]=='C'&&p[i+4]==1&&z>=15){v->profile=p[i+5];v->level=p[i+7];}
  if(p[i]=='a'&&p[i+1]=='v'&&p[i+2]=='c'&&p[i+3]=='1'&&z>=86&&i+32<=n){v->width=p[i+28]*256+p[i+29];v->height=p[i+30]*256+p[i+31];}
 }
}
static inline int mv_probe_hardware(const MvProbe*v){return (v->profile==66||v->profile==77)&&v->level>0&&v->level<=30&&v->width>0&&v->width<=480&&v->height>0&&v->height<=272;}
#endif
