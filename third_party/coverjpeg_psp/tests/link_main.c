#include "coverjpeg.h"
static unsigned char out[192*192*4];
int main(void) { int w=0,h=0; const unsigned char bad[4]={255,216,255,217}; return coverjpeg_decode_rgba_scaled(bad,4,out,sizeof(out),192,&w,&h); }
