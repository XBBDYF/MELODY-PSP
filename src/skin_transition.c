#include "skin_transition.h"
#include "skin_runtime.h"
#include "skin_page_turn.h"
#include "gfx.h"
#include <string.h>
#include <math.h>
typedef struct{int x,y,w,h;}Plane;
typedef struct{int phase,old_page,new_page,section,top,bottom;float started;Plane planes[12];int count;}Transition;
static Transition g_turn;
enum{TURN_NONE,TURN_CLOSE,TURN_WAIT,TURN_OPEN};
static int enabled(void){const SkinStyle*s=skin_runtime_style();return skin_runtime_active()&&!strncmp(s->id,"metro",5);}
static int supported(int page){return page>SC_BOOT&&page<SC_COUNT&&page!=SC_MV;}
static void add(int x,int y,int w,int h){int bottom=y+h;if(y<g_turn.top)y=g_turn.top;if(bottom>g_turn.bottom)bottom=g_turn.bottom;h=bottom-y;if(h>0&&g_turn.count<12){Plane*p=&g_turn.planes[g_turn.count++];p->x=x;p->y=y;p->w=w;p->h=h;}}
static void geometry(int page){
 g_turn.count=0;
 if(g_turn.section){for(int i=0;i<6;++i){int y=g_turn.top+(g_turn.bottom-g_turn.top)*i/6;int end=g_turn.top+(g_turn.bottom-g_turn.top)*(i+1)/6;add(0,y,480,end-y);}return;}
 switch(page){
 case SC_HOME:add(16,20,448,43);add(16,68,218,106);add(240,68,106,106);add(352,68,106,106);add(16,180,218,50);add(240,180,106,50);add(352,180,50,50);add(408,180,50,50);break;
 case SC_GRID:case SC_SQUARE:add(16,20,448,52);for(int i=0;i<6;i++)add(16+i%3*150,77+i/3*76,140,68);break;
 case SC_PLAYER:add(15,20,449,42);add(17,67,154,154);add(181,64,283,83);add(181,147,283,86);break;
 case SC_LYRICS:add(16,20,448,52);add(16,75,448,50);add(16,125,448,50);add(16,175,448,60);break;
 case SC_SEARCH:add(16,20,448,42);add(16,63,448,32);add(16,98,448,40);add(16,140,448,31);add(16,171,448,31);add(16,202,448,33);break;
 case SC_MINE:add(16,20,448,42);add(16,64,448,62);add(16,134,448,33);add(16,167,448,33);add(16,200,448,35);break;
 case SC_COMMENTS:add(16,20,448,54);add(5,76,459,52);add(5,128,459,52);add(5,180,459,55);break;
 case SC_LOGIN:case SC_SKINS:add(16,20,448,42);add(16,64,218,170);add(240,64,224,170);break;
 default:add(16,20,448,52);for(int i=0;i<4;i++)add(16,76+i*39,448,39);break;
 }
}
static unsigned short background(void){const SkinStyle*s=skin_runtime_style();return s->dark?gfx_rgb(8,8,8):gfx_rgb(255,255,255);}
static float length(void){return .22f+(g_turn.count>0?g_turn.count-1:0)*.022f;}
static float clamp01(float p){return p<0?0:p>1?1:p;}
void skin_transition_reset(void){memset(&g_turn,0,sizeof(g_turn));skin_page_release();}
void skin_transition_cancel(void){skin_transition_reset();}
int skin_transition_active(void){return g_turn.phase!=TURN_NONE;}
int skin_transition_waiting(void){return g_turn.phase==TURN_WAIT;}
void skin_transition_begin_wait(int page,float time){
 if(!enabled()||!supported(page)||g_turn.phase)return;
 if(!skin_page_capture())return;
 g_turn.phase=TURN_CLOSE;g_turn.old_page=page;g_turn.new_page=-1;g_turn.started=-1;g_turn.top=20;g_turn.bottom=236;geometry(page);
}
void skin_transition_wait_section(int page,int top,int bottom,float time){
 if(!enabled()||!supported(page))return;
 if(top<0)top=0;
 if(bottom>272)bottom=272;
 if(top>=bottom)return;
 /* A new user change replaces an older same-page operation, using the
  * currently visible frame. Full navigation retains ownership until done. */
 if(g_turn.phase&&!g_turn.section)return;
 if(g_turn.phase)skin_transition_reset();
 if(!skin_page_capture())return;
 g_turn.phase=TURN_CLOSE;g_turn.old_page=page;g_turn.new_page=-1;g_turn.started=-1;
 g_turn.section=1;g_turn.top=top;g_turn.bottom=bottom;geometry(page);
}
void skin_transition_ready_section(int page,float time){
 (void)time;
 if(g_turn.section&&g_turn.old_page==page&&(g_turn.phase==TURN_CLOSE||g_turn.phase==TURN_WAIT))g_turn.new_page=page;
}
void skin_transition_request_section(int page,int top,int bottom,float time){
 skin_transition_wait_section(page,top,bottom,time);skin_transition_ready_section(page,time);
}
void skin_transition_request(int old_page,int new_page,float time){
 if(old_page==new_page)return;
 if(!enabled()||!supported(old_page)||!supported(new_page)){skin_transition_reset();return;}
 if(g_turn.section)skin_transition_reset();
 if(g_turn.phase==TURN_NONE){skin_transition_begin_wait(old_page,time);}
 if(g_turn.phase){g_turn.new_page=new_page;if(g_turn.phase==TURN_OPEN){skin_transition_reset();skin_transition_begin_wait(old_page,time);g_turn.new_page=new_page;}}
}
void skin_transition_draw(int page,float time){
 if(!g_turn.phase)return;
 if(!enabled()||!supported(page)||!skin_page_active()){skin_transition_reset();return;}
 /* Navigation can synchronously load a catalog before the first draw.
  * Charge closing time only after the captured old page is presented. */
 if(g_turn.phase==TURN_CLOSE&&g_turn.started<0)g_turn.started=time;
 if(g_turn.phase==TURN_CLOSE&&time-g_turn.started>=length())g_turn.phase=TURN_WAIT;
 if(g_turn.phase==TURN_WAIT&&g_turn.new_page==page){
  /* Caller has rendered the actual destination already. Replace old snapshot
   * only now: no second full-page allocation and no fake data-ready timer. */
  if(!skin_page_capture()){skin_transition_reset();return;}
  geometry(page);g_turn.phase=TURN_OPEN;g_turn.started=time;
 }
 gfx_reset_clip();
 gfx_fill_rect(0,g_turn.top,480,g_turn.bottom-g_turn.top,background());
 if(g_turn.phase==TURN_WAIT)return;
 int opening=g_turn.phase==TURN_OPEN;
 float elapsed=time-g_turn.started;
 for(int i=g_turn.count-1;i>=0;--i){
  Plane*p=&g_turn.planes[i];float progress=clamp01((elapsed-i*.022f)/.22f);float eased=1-powf(1-progress,3);
  skin_page_fold(opening?1-eased:eased,p->x,p->y,p->w,p->h);
 }
 if(opening&&elapsed>=length())skin_transition_reset();
}
