"""Execute native navigation state machine for every application page and returns."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/skin-transition';O.mkdir(exist_ok=True)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "skin_runtime.h"
#include "skin_transition.h"
static SkinStyle style;static int snapshots,folds,free_count,live,last_drawn,fail_capture,region_top=0,region_bottom=272;static float last_p;static int partial_folds;
const SkinStyle*skin_runtime_style(void){return &style;}int skin_runtime_active(void){return style.id[0]!=0;}
int skin_page_capture(void){if(fail_capture)return 0;snapshots++;live=1;return 1;}void skin_page_release(void){live=0;free_count++;}int skin_page_active(void){return live;}
void skin_page_fold(float p,int x,int y,int w,int h){assert(x>=0&&y>=region_top&&x+w<=480&&y+h<=region_bottom);folds++;last_p=p;if(p>0&&p<1)partial_folds++;}
unsigned short gfx_rgb(int r,int g,int b){return (r<<8)|(g<<4)|b;}void gfx_reset_clip(void){}
void gfx_fill_rect(int x,int y,int w,int h,unsigned short color){assert(x==0&&w==480&&y>=region_top&&y+h<=region_bottom);(void)color;last_drawn++;}
int main(void){strcpy(style.id,"metro");style.dark=1;
 /* Slow synchronous catalog loading must not consume the unseen old-page close. */
 skin_transition_begin_wait(SC_HOME,1);int slow_capture=snapshots;
 skin_transition_request(SC_HOME,SC_SKINS,10);int slow_folds=folds;
 skin_transition_draw(SC_SKINS,10);
 assert(snapshots==slow_capture&&folds>slow_folds&&last_p==0&&!skin_transition_waiting());
 int slow_partial=partial_folds;skin_transition_draw(SC_SKINS,10.3f);
 assert(snapshots==slow_capture&&partial_folds>slow_partial);
 skin_transition_draw(SC_SKINS,11);assert(snapshots==slow_capture+1);
 skin_transition_draw(SC_SKINS,12);assert(!skin_transition_active()&&!live);
 int pages[]={SC_WIFI,SC_HOME,SC_LIST,SC_PLAYER,SC_LYRICS,SC_SEARCH,SC_DOWNLOAD,SC_CACHE,SC_SETTINGS,SC_HELP,SC_LOGIN,SC_GRID,SC_SQUARE,SC_COMMENTS,SC_MINE,SC_ALT,SC_SKINS};
 for(unsigned i=0;i<sizeof(pages)/sizeof(pages[0]);++i){int dest=pages[i];if(dest==SC_HOME)continue;skin_transition_reset();int old=snapshots;skin_transition_request(SC_HOME,dest,1);assert(skin_transition_active()&&snapshots==old+1);skin_transition_draw(dest,1.1f);assert(folds>0);skin_transition_draw(dest,2);assert(snapshots==old+2);skin_transition_draw(dest,3);assert(!skin_transition_active()&&!live);skin_transition_request(dest,SC_HOME,4);assert(skin_transition_active());skin_transition_draw(SC_HOME,4);skin_transition_draw(SC_HOME,5);skin_transition_draw(SC_HOME,6);assert(!skin_transition_active());}
 skin_transition_begin_wait(SC_HOME,10);skin_transition_draw(SC_HOME,10);int captured=snapshots;for(int i=0;i<120;i++){skin_transition_draw(SC_HOME,11+i*.033f);assert(skin_transition_waiting()&&snapshots==captured);}skin_transition_request(SC_HOME,SC_LIST,16);skin_transition_draw(SC_LIST,16);assert(snapshots==captured+1);skin_transition_draw(SC_LIST,17);assert(!skin_transition_active());
 skin_transition_begin_wait(SC_SEARCH,20);skin_transition_cancel();assert(!skin_transition_active()&&!live);skin_transition_request(SC_PLAYER,SC_MV,21);assert(!skin_transition_active());strcpy(style.id,"ocean");int before=snapshots;skin_transition_request(SC_HOME,SC_SETTINGS,22);assert(!skin_transition_active()&&snapshots==before);
 strcpy(style.id,"metro");region_top=76;region_bottom=230;skin_transition_wait_section(SC_GRID,76,230,30);captured=snapshots;
 skin_transition_draw(SC_GRID,30.1f);
 for(int i=0;i<120;++i){skin_transition_draw(SC_GRID,31+i*.033f);assert(skin_transition_waiting()&&snapshots==captured);}
 skin_transition_ready_section(SC_LIST,35);skin_transition_draw(SC_GRID,35);assert(skin_transition_waiting());
 skin_transition_ready_section(SC_GRID,35);skin_transition_draw(SC_GRID,35);assert(snapshots==captured+1&&!skin_transition_waiting());
 skin_transition_ready_section(SC_GRID,35.1f);skin_transition_draw(SC_GRID,36);assert(!skin_transition_active()&&!live);
 region_top=20;region_bottom=236;skin_transition_request_section(SC_SETTINGS,20,236,40);captured=snapshots;skin_transition_draw(SC_SETTINGS,40.1f);assert(snapshots==captured);skin_transition_draw(SC_SETTINGS,41);assert(snapshots==captured+1);skin_transition_draw(SC_SETTINGS,42);assert(!skin_transition_active());
 skin_transition_wait_section(SC_GRID,76,230,43);skin_transition_request(SC_GRID,SC_SKINS,43.1f);skin_transition_draw(SC_SKINS,43.1f);skin_transition_draw(SC_SKINS,44);skin_transition_draw(SC_SKINS,45);assert(!skin_transition_active()&&!live);
 fail_capture=1;skin_transition_request_section(SC_SETTINGS,20,236,46);assert(!skin_transition_active()&&!live);fail_capture=0;
 skin_transition_request_section(SC_SETTINGS,20,236,47);skin_transition_draw(SC_SETTINGS,47);fail_capture=1;skin_transition_draw(SC_SETTINGS,48);assert(!skin_transition_active()&&!live);fail_capture=0;
 skin_transition_wait_section(SC_GRID,76,230,49);style.id[0]=0;skin_transition_draw(SC_GRID,50);assert(!skin_transition_active()&&!live);
 strcpy(style.id,"metro");captured=snapshots;skin_transition_wait_section(SC_GRID,230,76,51);assert(!skin_transition_active()&&snapshots==captured);
 puts("PASS native every-page entry/return, clipped section long wait/ready once, same-page reveal, interruption, capture failure, reset and exclusions");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Isrc',O/'test.c',R/'src/skin_transition.c','-lm','-o',exe],cwd=R,check=True);subprocess.run([exe],check=True)
