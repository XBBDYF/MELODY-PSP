#ifndef MELODY_MV_STREAM_CONTRACT_H
#define MELODY_MV_STREAM_CONTRACT_H
#include <stdint.h>
#define MV_STREAM_READ_ERROR (-1)
#define MV_STREAM_READ_WAIT (-2)
static inline int mv_stream_idle_expired(uint32_t now,uint32_t last_data){return (uint32_t)(now-last_data)>=20000000u;}
/* Return a positive available prefix,0 only for confirmed complete EOF. */
static inline int mv_stream_read_window(int64_t offset,int request,int64_t available,int64_t total,int done,int error,int cancelled){
 if(offset<0||request<=0||available<0||total<=0||available>total||offset>total||cancelled||error)return MV_STREAM_READ_ERROR;
 if(offset<available){int64_t n=available-offset;return n<request?(int)n:request;}
 if(done)return available==total?0:MV_STREAM_READ_ERROR;
 return MV_STREAM_READ_WAIT;
}
static inline int64_t mv_stream_fixture_available(int64_t total,uint64_t elapsed_us,unsigned initial,unsigned bytes_per_second){
 if(total<=0)return 0;
 if((uint64_t)total<=initial)return total;
 uint64_t remain=(uint64_t)total-initial;
 if(bytes_per_second && elapsed_us>=((remain+bytes_per_second-1)/bytes_per_second)*1000000ull)return total;
 uint64_t grown=elapsed_us/1000000ull*bytes_per_second+(elapsed_us%1000000ull)*bytes_per_second/1000000ull;
 return grown>=remain?total:(int64_t)(initial+grown);
}
#endif
