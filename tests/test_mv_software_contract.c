#include <assert.h>
#include "../src/mv_settings_contract.h"
int main(void){
    assert(mv_decoder_value(0)==MV_DECODE_SOFTWARE);
    assert(mv_decoder_value(1)==MV_DECODE_HARDWARE);
    assert(mv_decoder_value(7)==MV_DECODE_SOFTWARE);
    assert(mv_stream_prefetch_bytes(8LL*1024*1024,0)==256*1024);
    assert(mv_stream_prefetch_bytes(8LL*1024*1024,1)==512*1024);
    assert(mv_stream_prefetch_bytes(10000,0)==10000);
    assert(mv_stream_prefetch_bytes(-1,0)==256*1024);
    assert(mv_stream_needs_range(900000,262144,1000000));
    assert(!mv_stream_needs_range(262145,262144,1000000));
    assert(!mv_stream_needs_range(1000000,262144,1000000));
    assert(!mv_stream_needs_range(-1,262144,1000000));
    assert(!mv_stream_needs_range(900000,262144,-1));
    return 0;
}
