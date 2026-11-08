#ifndef MELODY_MV_SETTINGS_CONTRACT_H
#define MELODY_MV_SETTINGS_CONTRACT_H
/* These filenames are also read by the small PRX loader. */
#define MV_DECODER_FILE "mvdecoder.txt"
#define MV_SOFT_QUALITY_FILE "mvquality.txt"
#define MV_STREAM_FILE "mvstream.txt"
enum { MV_DECODE_SOFTWARE=0, MV_DECODE_HARDWARE=1, MV_DECODE_AUTO=2 };
enum { MV_SOFT_STANDARD=0, MV_SOFT_FAST=1 };
static inline int mv_decoder_value(int value) { return value>=0&&value<=2 ? value : MV_DECODE_SOFTWARE; }
static inline long long mv_stream_prefetch_bytes(long long total, int hardware) {
    long long need=hardware ? 512*1024 : 256*1024;
    return total>0 && total<need ? total : need;
}
static inline int mv_stream_needs_range(long long off, long long available, long long total) {
    return off>=0 && total>0 && off<total && off>available && off-available>128*1024;
}
static inline int mv_buffer_percent(long long available,long long target){
    if(available<=0||target<=0)return 0;
    if(available>=target)return 100;
    return (int)(available*100/target);
}
#endif
