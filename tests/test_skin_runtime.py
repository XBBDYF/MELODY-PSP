"""Compile actual Lua5.4+production theme runtime; test hot switch, actions and budgets."""
from pathlib import Path
import subprocess,tempfile,json,os
R=Path(__file__).resolve().parents[1];O=R/'.tmp/skin-runtime-host';O.mkdir(exist_ok=True)
(O/'pspkernel.h').write_text('unsigned sceKernelGetSystemTimeLow(void);\n',encoding='ascii')
(O/'pspctrl.h').write_text(''.join('#define PSP_CTRL_'+n+' '+str(1<<i)+'u\n'for i,n in enumerate(['UP','DOWN','LEFT','RIGHT','CIRCLE','CROSS','SQUARE','TRIANGLE','SELECT','LTRIGGER','RTRIGGER'])),encoding='ascii')
(O/'pspiofilemgr.h').write_text('#define PSP_O_RDONLY 0\n#define PSP_O_WRONLY 1\n#define PSP_O_CREAT 256\n#define PSP_O_TRUNC 512\n#define PSP_SEEK_END 2\n#define PSP_SEEK_SET 0\nint sceIoOpen(const char*,int,int);long long sceIoLseek(int,long long,int);int sceIoRead(int,void*,int);int sceIoWrite(int,const void*,int);int sceIoClose(int);int sceIoRemove(const char*);int sceIoRename(const char*,const char*);\n',encoding='ascii')
(O/'psprtc.h').write_text('typedef struct {int hour,minute;} ScePspDateTime;int sceRtcGetCurrentClockLocalTime(ScePspDateTime*);\n',encoding='ascii')
(O/'psppower.h').write_text('int scePowerGetBatteryLifePercent(void);\n',encoding='ascii')
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <fcntl.h>
#include <io.h>
#include "app.h"
#include "skin_runtime.h"
#include "skin_options.h"
#include "dl.h"
#include "cache.h"
#include "mv.h"
#include "audio.h"
#include "psprtc.h"
AppState g_app;SongList g_list;int g_volume=80;
static unsigned ticks,tick_step=5,native_draw_cost;static int draws,captures,turns,releases;static float last_turn;static unsigned short last_text_color;static char last_text[512];
void skin_music_fx_draw(int s,float t,int l,unsigned short c){(void)s;(void)t;(void)l;(void)c;}
void skin_wave_draw(float t,unsigned short c){(void)t;(void)c;}void skin_wave_release(void){}
void skin_options_load(const char*r,const char*i){(void)r;(void)i;}void skin_options_reset(void){}int skin_options_count(void){return 0;}const SkinOption*skin_options_at(int i){(void)i;return NULL;}int skin_options_set(int i,int v){(void)i;(void)v;return 0;}int skin_options_value(const char*i,int f){(void)i;return f;}
void lyric_draw_text(int x,int y,int w,const char*t,int f,unsigned short b,unsigned short a,int i,int p,int o){(void)x;(void)y;(void)w;(void)t;(void)f;(void)b;(void)a;(void)i;(void)p;(void)o;}
float lyric_scroll_position(float v,float target,float dt,int gap){(void)v;(void)dt;(void)gap;return target;}
int ui_skin_icon(const char*n,int x,int y,int size,unsigned short c){(void)n;(void)x;(void)y;(void)size;(void)c;return 1;}
int store_disc_player(void){return 0;}
int app_download_selected(long long id){(void)id;return 0;}
int audio_level(void){return 0;}
int audio_volume(void){return 43;}int store_show_speed(void){return 0;}int sysi_used_kb(void){return 18000;}int sysi_free_kb(void){return 14000;}
int audio_cache_pct(void){return 37;}int audio_buffered_ms(void){return 12000;}int net_speed_kbps(void){return 28;}long long cache_total_bytes(void){return 12345678;}int sysi_ms0_free_mb(void){return 256;}int api_pending_download_count(void){return 2;}
const CardList*api_discovery_cards(void){static CardList discovery;return &discovery;}
const CardList*api_search_entities(void){static CardList entities;return &entities;}

int sceRtcGetCurrentClockLocalTime(ScePspDateTime*t){t->hour=12;t->minute=30;return 0;}int scePowerGetBatteryLifePercent(void){return 90;}
int cache_count(void){return 12;}int local_count(void){return 4;}const char*local_name(int i){(void)i;return "local.mp3";}int store_fav_count(void){return 3;}int store_recent_count(void){return 5;}int dl_queue_count(void){return 2;}int mv_dl_snapshot(MvDlItem*out,int n){(void)out;(void)n;return 0;}
int api_suggest_count(void){return 0;}const char*api_suggest(int i){(void)i;return "";}
int api_qr_state(void){return 801;}const unsigned char*api_qr_matrix(int*d){*d=0;return NULL;}
int api_logged_in(void){return 0;}const char*api_login_name(void){return "";}const char*api_search_category_name(int k){(void)k;return "songs";}
int cache_snapshot(CacheEntry*out,int n){(void)out;(void)n;return 0;}
static Song daily_fixture;static CardList home_fixture;static char bound_urls[4][208];static int bound_count,avatar_requests,enable_fixture;
void api_request_daily_preview(void){}
const Song*api_daily_preview(void){return daily_fixture.id?&daily_fixture:NULL;}const CardList*api_home_cards(void){return &home_fixture;}const char*api_user_avatar(void){return enable_fixture?"fixture-avatar":"";}int api_avatar_slot(const char*u){if(enable_fixture){assert(!strcmp(u,"fixture-avatar"));avatar_requests++;return 0;}return -1;}const unsigned short*api_avatar_px(int slot){static unsigned short px[48*48];return enable_fixture&&slot==0?px:NULL;}
int api_thumb_slot(const char*u){if(enable_fixture&&bound_count<4)snprintf(bound_urls[bound_count++],208,"%s",u);return -1;}const unsigned short*api_thumb_px(int slot){(void)slot;return NULL;}void api_request_thumbs(void){}
const unsigned short*api_art(int*w,int*h){*w=*h=0;return NULL;}
unsigned short*gfx_backbuffer(void){static unsigned short frame[512*272];return frame;}
int gfx_text_w(int font,const char*t){(void)font;return strlen(t)*6;}
void gfx_blit565_cover(int x,int y,int w,int h,const unsigned short*p,int sw,int sh){(void)x;(void)y;(void)w;(void)h;(void)p;(void)sw;(void)sh;}
unsigned skin_assets_decode_us(void){return 0;}unsigned skin_assets_memory_bytes(void){return 0;}
int skin_page_capture(void){captures++;return 1;}void skin_page_turn(float p,int t,int b,unsigned short c){last_turn=p;turns++;(void)t;(void)b;(void)c;}void skin_page_fold(float p,int x,int y,int w,int h){last_turn=p;turns++;(void)x;(void)y;(void)w;(void)h;}void skin_page_release(void){releases++;}
static int nav_waits;
void skin_transition_begin_wait(int p,float t){(void)p;(void)t;nav_waits++;}void skin_transition_reset(void){}
static float drawn_disc_angle;static int drawn_disc_count;
int skin_disc_draw(int x,int y,int size,float angle){(void)x;(void)y;(void)size;drawn_disc_angle=angle;drawn_disc_count++;return 0;}void skin_disc_release(void){}
void skin_assets_set_root(const char*r){(void)r;}void skin_assets_release(void){}void skin_assets_begin_frame(void){}
int skin_assets_image(const char*id,int x,int y,int w,int h,int alpha){(void)id;(void)x;(void)y;(void)w;(void)h;(void)alpha;return 0;}
int skin_assets_probe(const char*r,char*e,int n){(void)r;(void)e;(void)n;return 1;}
void skin_sound_set_root(const char*r){(void)r;}void skin_sound_release(void){}int skin_sound_play(const char*id){(void)id;return 0;}

const Lyric*api_lyric(void){static Lyric lyric;return &lyric;}
const CommentList*api_comments(void){static CommentList comments;return &comments;}
int dl_queue_snapshot(DlItem*out,int max){(void)out;(void)max;return 0;}
unsigned sceKernelGetSystemTimeLow(void){return ticks+=tick_step;}
int sceIoOpen(const char*p,int flags,int mode){(void)mode;if(flags)return -1;return _open(p,_O_RDONLY|_O_BINARY);}
long long sceIoLseek(int fd,long long o,int where){return _lseeki64(fd,o,where);}
int sceIoRead(int fd,void*p,int n){return _read(fd,p,n);}
int sceIoWrite(int fd,const void*p,int n){return _write(fd,p,n);}
int sceIoClose(int fd){return _close(fd);}
int sceIoRemove(const char*p){(void)p;return -1;}
int sceIoRename(const char*a,const char*b){(void)a;(void)b;return -1;}
void mlog(const char*format,...){(void)format;}
unsigned short gfx_rgb(int r,int g,int b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
void gfx_fill_rect(int x,int y,int w,int h,unsigned short c){(void)x;(void)y;(void)w;(void)h;(void)c;ticks+=native_draw_cost;draws++;}
void gfx_fill_rect_a(int x,int y,int w,int h,unsigned short c,int alpha){(void)alpha;gfx_fill_rect(x,y,w,h,c);}
void gfx_hgrad(int x,int y,int w,int h,unsigned short a,unsigned short b){(void)a;(void)b;gfx_fill_rect(x,y,w,h,0);}
void gfx_fill_round_grad(int x,int y,int w,int h,int r,unsigned short a,unsigned short b){(void)r;(void)a;(void)b;gfx_fill_rect(x,y,w,h,0);}
void gfx_fill_round(int x,int y,int w,int h,int radius,unsigned short c){(void)radius;gfx_fill_rect(x,y,w,h,c);}
void gfx_circle(int x,int y,int r,unsigned short c){gfx_fill_rect(x,y,r,r,c);}
void gfx_text(int size,int x,int y,const char*t,unsigned short c){(void)size;(void)t;gfx_fill_rect(x,y,1,1,c);}
static int saw_speed,saw_mem,saw_vol43;
void gfx_text_ellipsis(int size,int x,int y,int w,const char*t,unsigned short c){if(strstr(t,"KB/s"))saw_speed++;if(strstr(t,"M"))saw_mem++;if(strstr(t,"43%"))saw_vol43++;(void)w;snprintf(last_text,sizeof(last_text),"%s",t);last_text_color=c;gfx_text(size,x,y,t,c);}
void gfx_set_clip(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}
void gfx_reset_clip(void){}
static int module(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;draws++;return 1;}
int main(int argc,char**argv){char err[192],action[48],arg[128];if(argc==3&&!strcmp(argv[1],"--validate")){int r=skin_runtime_validate(argv[2],err,sizeof(err));printf("%s %s\n",r==0?"VALID":"INVALID",r==0?argv[2]:err);return r<0;}if(argc==3&&!strcmp(argv[1],"--disc-animation")){Song song={0};strcpy(song.name,"Disc track");song.id=432;assert(skin_runtime_apply(argv[2],err,sizeof(err))==0);skin_runtime_set_playback(&song,1,1000,90000);g_app.paused=0;assert(skin_runtime_draw(SC_HOME,1,.05f));float a=drawn_disc_angle;assert(skin_runtime_draw(SC_HOME,1.05f,.05f));assert(drawn_disc_angle>a);g_app.paused=1;a=drawn_disc_angle;assert(skin_runtime_draw(SC_HOME,1.1f,.05f));assert(drawn_disc_angle==a&&drawn_disc_count==3);puts("PASS generated disc Lua rotates during play and preserves angle during pause");skin_runtime_reset();return 0;}
if(argc==3&&!strcmp(argv[1],"--source-bindings")){
 enable_fixture=1;daily_fixture.id=111;strcpy(daily_fixture.art,"daily-cover");home_fixture.count=2;strcpy(home_fixture.items[0].cover,"recommend-one");strcpy(home_fixture.items[1].cover,"recommend-two");
 assert(skin_runtime_apply(argv[2],err,sizeof(err))==0);assert(skin_runtime_draw(SC_HOME,1,.016f));assert(bound_count==2&&!strcmp(bound_urls[0],"daily-cover")&&!strcmp(bound_urls[1],"recommend-two")&&avatar_requests==1);puts("PASS actual tile_cover chooses daily snapshot and requested recommendation index; avatar uses account cache");skin_runtime_reset();return 0;}
if(argc==3&&!strcmp(argv[1],"--wait-and-lyrics")){
 assert(skin_runtime_apply(argv[2],err,sizeof(err))==0);g_app.time=1;assert(skin_runtime_draw(SC_HOME,1,.016f));skin_runtime_input(SC_HOME,1u<<4,0,.016f);assert(nav_waits==1);assert(skin_runtime_action(action,sizeof(action),arg,sizeof(arg))&&!strcmp(action,"daily"));
 for(int f=0;f<120;++f){g_app.time=1.6+f*.033f;assert(skin_runtime_draw(SC_HOME,g_app.time,.033f));assert(nav_waits==1);assert(!skin_runtime_action(action,sizeof(action),arg,sizeof(arg)));}
 assert(skin_runtime_draw(SC_LIST,6,.016f));
 g_app.time=8;saw_speed=saw_vol43=0;assert(skin_runtime_draw(SC_PLAYER,8,.016f));assert(saw_vol43>0&&saw_speed==0);
 assert(skin_runtime_input(SC_PLAYER,1u<<7,0,.016f)==1);assert(skin_runtime_draw(SC_PLAYER,8.3f,.016f));assert(skin_runtime_input(SC_PLAYER,1u<<6,0,.016f)==1);assert(skin_runtime_action(action,sizeof(action),arg,sizeof(arg))&&!strcmp(action,"toggle_translation"));
 g_app.time=9;assert(skin_runtime_input(SC_PLAYER,1u<<7,0,.016f)==1);assert(skin_runtime_draw(SC_PLAYER,9.3f,.016f));assert(skin_runtime_input(SC_PLAYER,1u<<6,0,.016f)==1);assert(skin_runtime_action(action,sizeof(action),arg,sizeof(arg))&&!strcmp(action,"toggle_translation"));
 g_app.time=10;assert(skin_runtime_input(SC_PLAYER,1u<<7,0,.016f)==1);assert(skin_runtime_draw(SC_PLAYER,10.3f,.016f));assert(skin_runtime_input(SC_PLAYER,1u<<6,0,.016f)==0);
 puts("PASS one old-page capture, one action, four seconds data wait without replay, one ready reveal; Triangle three states and Square translation only in lyrics states");skin_runtime_reset();return 0;}
if(argc==3&&!strcmp(argv[1],"--home-categories")){
 assert(skin_runtime_apply(argv[2],err,sizeof(err))==0);g_app.time=1;assert(skin_runtime_draw(SC_HOME,1,.016f));
 skin_runtime_input(SC_HOME,1u<<10,0,.016f);skin_runtime_input(SC_HOME,1u<<10,0,.016f);assert(skin_runtime_draw(SC_HOME,1.02f,.016f));assert(!skin_runtime_action(action,sizeof(action),arg,sizeof(arg)));
 skin_runtime_input(SC_HOME,1u<<4,0,.016f);skin_runtime_draw(SC_HOME,g_app.time,.016f);g_app.time=1.7;skin_runtime_input(SC_HOME,0,0,.016f);assert(skin_runtime_action(action,sizeof(action),arg,sizeof(arg))&&!strcmp(action,"favorites"));
 puts("PASS R twice selects My section without navigating to Search; first tile opens actual favorites after animation");skin_runtime_reset();return 0;}
if(argc==3&&!strcmp(argv[1],"--page-isolation")){
 assert(skin_runtime_apply(argv[2],err,sizeof(err))==0);assert(skin_runtime_draw(SC_HOME,1,.016f)==1);
 assert(skin_runtime_draw(SC_PLAYER,2,.016f)==0);assert(skin_runtime_take_notice()==1);assert(skin_runtime_take_notice()==0);
 assert(skin_runtime_draw(SC_HOME,3,.016f)==1);assert(skin_runtime_draw(SC_SEARCH,4,.016f)==1);assert(skin_runtime_input(SC_HOME,16,0,.016f)==1);
 assert(skin_runtime_action(action,sizeof(action),arg,sizeof(arg))&&!strcmp(action,"search"));assert(skin_runtime_input(SC_PLAYER,16,0,.016f)==0);
 assert(skin_runtime_draw(SC_PLAYER,5,.016f)==0);assert(skin_runtime_take_notice()==0);skin_runtime_reset();puts("PASS one failing page cannot retire other page renderers or inputs; fallback notice delivered once");return 0;
}
if(argc==3&&!strcmp(argv[1],"--slow-device")){
 assert(skin_runtime_apply(argv[2],err,sizeof(err))==0);tick_step=20000;native_draw_cost=800;g_app.settings_sel=3;
 int pages[]={SC_SETTINGS,SC_MINE,SC_HOME,SC_SEARCH,SC_PLAYER,SC_CACHE};
 for(int i=0;i<6;++i){int rc=skin_runtime_draw(pages[i],i+1,.033f);if(rc<=0){fprintf(stderr,"SLOW_DEVICE page=%d fell back: %s\n",pages[i],skin_runtime_error());return 1;}}
 assert(skin_runtime_error()[0]==0);puts("PASS slow-device rendering/preemption never deactivates Metro");skin_runtime_reset();return 0;}
 assert(argc==8);SkinModule modules[]={{"songs",module,NULL},{"account",module,NULL}};skin_runtime_register(modules,2);
 assert(skin_runtime_validate(argv[1],err,sizeof(err))==0);assert(skin_runtime_apply(argv[1],err,sizeof(err))==0);assert(skin_runtime_active()&&!strcmp(skin_runtime_style()->id,"ocean"));
 assert(skin_runtime_validate(argv[2],err,sizeof(err))==0);assert(skin_runtime_apply(argv[2],err,sizeof(err))==0);assert(skin_runtime_draw(SC_HOME,1,.016f)>0&&draws>0);for(int frame=0;frame<900;++frame)assert(skin_runtime_draw(SC_HOME,frame/60.f,.016f)>0);g_app.time=20;skin_runtime_input(SC_HOME,1u<<3,1u<<3,.016f);for(int hold=1;hold<=10;++hold){g_app.time=20+hold*.016f;skin_runtime_input(SC_HOME,0,1u<<3,.016f);}skin_runtime_input(SC_HOME,1u<<4,0,.016f);skin_runtime_draw(SC_HOME,g_app.time,.016f);g_app.time+=.6f;skin_runtime_input(SC_HOME,0,0,.016f);assert(skin_runtime_action(action,sizeof(action),arg,sizeof(arg))&& !strcmp(action,"square"));
 assert(skin_runtime_apply(argv[3],err,sizeof(err))==0);assert(skin_runtime_draw(SC_HOME,1,.016f)>0);assert(!strcmp(skin_runtime_style()->id,"declarative"));skin_runtime_input(SC_HOME,1u<<4,0,.016f);assert(skin_runtime_action(action,sizeof(action),arg,sizeof(arg))&&!strcmp(action,"skins"));
 assert(skin_runtime_apply(argv[4],err,sizeof(err))<0);assert(!strcmp(skin_runtime_style()->id,"declarative"));
 assert(skin_runtime_validate(argv[5],err,sizeof(err))<0);assert(skin_runtime_validate(argv[6],err,sizeof(err))<0);
 assert(skin_runtime_apply(argv[7],err,sizeof(err))==0);int result=skin_runtime_draw(SC_HOME,1,.016f);assert(result==0&&skin_runtime_error()[0]);assert(!skin_runtime_draw(SC_HOME,2,.016f));
 skin_runtime_reset();assert(!skin_runtime_active());puts("PASS production Lua runtime: palette, animated page, action bridge, declarative widgets, bad apply preserves active skin, infinite loop/heap limits, callback failure falls back");return 0;}
'''

# 主题字体在宿主机测试里只需要桩：apply/release 走文件 I/O，不在本用例范围
FONT_STUBS = r"""
int skin_font_apply(const char *root){(void)root;return 0;}
void skin_font_release(void){}
int skin_font_bytes(void){return 0;}
const char *skin_font_last_error(void){return "";}
"""
(O/'test.c').write_text(code+FONT_STUBS,encoding='utf-8');names=(R/'tools/prxloader/lua_sources.txt').read_text(encoding='ascii').splitlines();exe=O/'runtime.exe';cmd=['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-O1','-Wall','-Wextra','-Wno-misleading-indentation','-DLUA_32BITS','-DLUAI_MAXCCALLS=40','-I'+str(O),'-I'+str(R/'src'),'-I'+str(R/'third_party/lua-5.4.7'),O/'test.c',R/'src/skin_runtime.c',R/'src/json.c',R/'src/ime.c',R/'src/search_history.c',*[R/n for n in names],'-lm','-o',exe]
subprocess.run(cmd,cwd=R,check=True,timeout=90)
with tempfile.TemporaryDirectory(prefix='melody-skin-runtime-')as td:
 p=Path(td);args=[R/'skin_release/melody/skins/ocean',R/'skin_release/melody/skins/metro']
 for id,body,script in [
 ('declarative',{'schema':1,'id':'declarative','name':'declarative','pages':{'home':{'widgets':[{'type':'rect','x':0,'y':0,'width':480,'height':272,'color':'#111111'},{'type':'text','x':20,'y':30,'width':200,'height':20,'text':'hello','color':'#EEEEEE','action':'skins'}]}}},None),
 ('bad',{'schema':1,'id':'bad','name':'bad','colors':{'accent':'not-a-color'}},None),
 ('loop',{'schema':1,'id':'loop','name':'loop','script':'skin.lua'},'while true do end'),
 ('memory',{'schema':1,'id':'memory','name':'memory','script':'skin.lua'},'local t={} for i=1,100000 do t[i]=string.rep("x",100) end return {}'),
 ('callback',{'schema':1,'id':'callback','name':'callback','script':'skin.lua'},'return { draw=function() while true do end end }')]:
  d=p/id;d.mkdir();(d/'skin.json').write_text(json.dumps(body),encoding='utf-8')
  if script:(d/'skin.lua').write_text(script,encoding='utf-8')
  args.append(d)
 subprocess.run([exe,*args],cwd=p,check=True,timeout=15)
 finalizer=p/'finalizer';finalizer.mkdir();(finalizer/'skin.json').write_text(json.dumps({'schema':1,'id':'finalizer','name':'finalizer','script':'skin.lua'}),encoding='utf-8');(finalizer/'skin.lua').write_text('return setmetatable({}, {__gc=function() while true do end end})',encoding='utf-8')
 v=subprocess.run([exe,'--validate',finalizer],cwd=p,capture_output=True,timeout=3);assert v.returncode!=0,'finalizers must not bypass the instruction hook'
 print('PASS finalizer hook bypass is rejected before metatable creation')
 bindings=p/'bindings';bindings.mkdir();(bindings/'skin.json').write_text(json.dumps({'schema':1,'id':'bindings','name':'bindings','script':'skin.lua'}),encoding='utf-8');(bindings/'skin.lua').write_text('return {draw=function() ui.tile_cover("daily",1,0,0,40,40);ui.tile_cover("recommendations",2,40,0,40,40);ui.avatar(80,0,40,40);return true end}',encoding='utf-8');subprocess.run([exe,'--source-bindings',bindings],cwd=p,check=True,timeout=5)

 isolated=p/'isolated';isolated.mkdir();(isolated/'skin.json').write_text(json.dumps({'schema':1,'id':'isolated','name':'isolated','script':'skin.lua'}),encoding='utf-8');(isolated/'skin.lua').write_text('return {draw=function(page) if page=="player" then error("broken player only") end ui.rect(0,0,20,20,"#FFFFFF");return true end,input=function() app.dispatch("search");return true end}',encoding='utf-8');subprocess.run([exe,'--page-isolation',isolated],cwd=p,check=True,timeout=5)



subprocess.run([exe,"--slow-device",R/"skin_release/melody/skins/metro"],cwd=R,check=True,timeout=15)

subprocess.run([exe,"--home-categories",R/"skin_release/melody/skins/metro"],cwd=R,check=True,timeout=15)

subprocess.run([exe,"--wait-and-lyrics",R/"skin_release/melody/skins/metro"],cwd=R,check=True,timeout=15)

if (R/".tmp/skin-editor-exports/editor_disc/skin.lua").exists():subprocess.run([exe,"--disc-animation",R/".tmp/skin-editor-exports/editor_disc"],cwd=R,check=True,timeout=10)
