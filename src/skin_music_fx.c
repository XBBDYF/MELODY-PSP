#include "skin_music_fx.h"
#include "gfx.h"
#include <math.h>
void skin_music_fx_draw(int style,float time,int level,unsigned short color){
 if(style<1||style>2||!isfinite(time))return;
 if(level<0)level=0;
 if(level>100)level=100;
 if(gfx_push_clip(12,58,468,234)<0)return;
 if(style==1){
  /* Ambient rain; measured music energy only modulates streak length/opacity. */
  for(int i=0;i<18;i++){float speed=35.f+(i%5)*8.f+level*.18f;float t=fmodf(time*speed+i*37.f,190.f);int x=20+(i*73)%438,y=48+(int)t,len=5+(i%4)*2+level/14;
   gfx_fill_rect_a(x,y,1,len,color,30+level/3);gfx_fill_rect_a(x-1,y+len-2,3,2,color,48+level/3);
  }
 }else{
  for(int i=0;i<28;i++){float shape=.25f+.75f*(.5f+.5f*cosf((i-13.5f)*.22f));int h=2+(int)(level*.75f*shape);int x=31+i*15;
   gfx_fill_rect_a(x,222-h,8,h,color,38+level/2);
  }
 }
 gfx_pop_clip();
}
