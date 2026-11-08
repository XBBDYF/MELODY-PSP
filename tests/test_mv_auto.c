#include <assert.h>
#include <string.h>
#include "../src/mv_settings_contract.h"
#include "../src/mv_probe.h"
int main(void){
 assert(mv_buffer_percent(512*1024,2*1024*1024)==25);
 assert(mv_buffer_percent(2*1024*1024,2*1024*1024)==100);
 assert(mv_buffer_percent(1000,500)==100);
 assert(mv_buffer_percent(-1,500)==0);
 assert(mv_buffer_percent(10,0)==0);
 assert(mv_decoder_value(2)==MV_DECODE_AUTO);assert(mv_decoder_value(9)==MV_DECODE_SOFTWARE);
 unsigned char b[128]={0};b[3]=86;memcpy(b+4,"avc1",4);b[32]=1;b[33]=224;b[34]=1;b[35]=14;
 b[91]=20;memcpy(b+92,"avcC",4);b[96]=1;b[97]=77;b[99]=30;
 MvProbe p={0};mv_probe_bytes(&p,b,sizeof(b));assert(p.width==480&&p.height==270&&p.profile==77&&mv_probe_hardware(&p));
 b[97]=100;p=(MvProbe){0};mv_probe_bytes(&p,b,sizeof(b));assert(!mv_probe_hardware(&p));
 b[97]=66;b[32]=2;b[33]=128;p=(MvProbe){0};mv_probe_bytes(&p,b,sizeof(b));assert(!mv_probe_hardware(&p));
 p=(MvProbe){0};mv_probe_bytes(&p,b,10);assert(!mv_probe_hardware(&p));
 return 0;
}
