#include "skin_runtime.h"
#include "skin_font.h"
#include "ime.h"
#include "search_history.h"
#include "json.h"
#include "gfx.h"
#include "store.h"
#include "api.h"
#include "dl.h"
#include "cache.h"
#include "local.h"
#include "skin_assets.h"
#include "skin_sound.h"
#include "skin_disc.h"
#include "skin_wave.h"
#include "skin_music_fx.h"
#include "lyric_motion.h"
#include "lyric_render.h"
#include "skin_options.h"
#include "ui.h"
#include "skin_page_turn.h"
#include "skin_transition.h"
#include "mv.h"
#include "audio.h"
#include "net.h"
#include "sysinfo.h"
#include "settings_order.h"
#include <psprtc.h>
#include <psppower.h>
#include "log.h"
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#define SKIN_HEAP_LIMIT (384*1024)
#define SKIN_JSON_LIMIT 24576
#define SKIN_SCRIPT_LIMIT 49152
#define SKIN_INSTRUCTIONS 90000
#define SKIN_SLOW_FRAME_US 50000u /* diagnostic only: wall time includes preemption */
#define SKIN_ACTIVE APP_DIR "skins/active.txt"
typedef struct{int page,x,y,w,h,font,radius;char type[16],text[160],binding[48],action[48],argument[128];unsigned short color;int set_color;}SkinWidget;
typedef struct{size_t bytes;unsigned instructions,begin,blocked_pages,last_slow_log,slow_frames,string_work;int drawing,dry,calls,failed,script,call_page;float call_time,call_dt;unsigned call_pressed,call_held;const char*event;SkinWidget*widgets;int widget_count,focused_widget,focus_page;unsigned pages_mask;lua_State*L;SkinStyle style;}SkinVM;
static SkinVM g_vm;
static const SkinModule*g_modules;static int g_module_count;
static float g_status_sampled_at=-2;static int g_status_battery;static char g_status_clock[16];
static int g_page;static const SongList*g_list_model;static const CardList*g_card_model;
static const CacheEntry*g_cache_model;static int g_cache_model_count;
static const Song*g_now_model;static int g_audio_state,g_position,g_duration;
static const char*const*g_setting_labels;static const int*g_setting_groups;static const char(*g_setting_values)[96];static int g_setting_count;
static char g_error[192],g_action[48],g_argument[128];
static int g_notice;
static const char*const color_names[]={"background","panel","text","secondary","muted","line","accent","hot","panel2","white","black","success","warning","row","shadow"};
static const char*const page_names[]={"boot","wifi","home","songs","player","lyrics","search","downloads","cache","settings","about","account","grid","square","comments","mine","video","sources","skins"};
static void*skin_alloc(void*ud,void*ptr,size_t old,size_t size){SkinVM*v=ud;if(!ptr)old=0;if(!size){free(ptr);v->bytes-=old;return NULL;}if(size>SKIN_HEAP_LIMIT||v->bytes-old>SKIN_HEAP_LIMIT-size)return NULL;void*p=realloc(ptr,size);if(p)v->bytes=v->bytes-old+size;return p;}
static SkinVM*vm(lua_State*L){return *(SkinVM**)lua_getextraspace(L);}
static void budget(lua_State*L,lua_Debug*ar){(void)ar;SkinVM*v=vm(L);v->instructions+=1000;if(v->instructions>SKIN_INSTRUCTIONS)luaL_error(L,"skin execution budget exceeded");}
static int color(const char*s,unsigned short*out){if(!s||strlen(s)!=7||s[0]!='#')return 0;unsigned value=0;for(int i=1;i<7;++i){unsigned char c=s[i];int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(n<0)return 0;value=value*16+n;}*out=gfx_rgb(value>>16,(value>>8)&255,value&255);return 1;}
static unsigned short lua_color(lua_State*L,int index){unsigned short c;if(lua_type(L,index)==LUA_TNUMBER){lua_Integer n=lua_tointeger(L,index);if(n<0||n>65535)luaL_error(L,"color must be #RRGGBB or RGB565");return(unsigned short)n;}if(!color(luaL_checkstring(L,index),&c))luaL_error(L,"invalid #RRGGBB color");return c;}
static int dimension(lua_State*L,int i,int min,int max){lua_Number value=luaL_checknumber(L,i);if(!isfinite((double)value)||value<min||value>max)luaL_error(L,"drawing coordinate outside limits");return(int)floor((double)value+.5);}
static int draw_admit(lua_State*L){SkinVM*v=vm(L);if(!v->drawing)return luaL_error(L,"drawing only allowed in draw()");if(++v->calls>320)return luaL_error(L,"too many drawing commands");return!v->dry;}
static int skin_rect(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544),w=dimension(L,3,0,960),h=dimension(L,4,0,544);unsigned short c=lua_color(L,5);if(draw_admit(L))gfx_fill_rect(x,y,w,h,c);return 0;}
static int skin_round(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544),w=dimension(L,3,0,960),h=dimension(L,4,0,544),r=dimension(L,5,0,128);unsigned short c=lua_color(L,6);if(draw_admit(L))gfx_fill_round(x,y,w,h,r,c);return 0;}
static int skin_text(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544);size_t n;const char*s=luaL_checklstring(L,3,&n);if(n>512)return luaL_error(L,"text limit512 bytes");int size=(int)luaL_optinteger(L,4,0);if(size<0||size>3)return luaL_error(L,"font index0..3");unsigned short c=lua_color(L,5);if(draw_admit(L))gfx_text(size,x,y,s,c);return 0;}
static int skin_line(lua_State*L){int x1=dimension(L,1,-480,960),y1=dimension(L,2,-272,544),x2=dimension(L,3,-480,960),y2=dimension(L,4,-272,544);unsigned short c=lua_color(L,5);int width=luaL_optinteger(L,6,1);if(width<1||width>8)return luaL_error(L,"line width1..8");if(!draw_admit(L))return 0;int dx=abs(x2-x1),sx=x1<x2?1:-1,dy=-abs(y2-y1),sy=y1<y2?1:-1,error=dx+dy;for(;;){gfx_fill_rect(x1-width/2,y1-width/2,width,width,c);if(x1==x2&&y1==y2)break;int e=error*2;if(e>=dy){error+=dy;x1+=sx;}if(e<=dx){error+=dx;y1+=sy;}}return 0;}
static int skin_label(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544),w=dimension(L,3,0,960);size_t n;const char*t=luaL_checklstring(L,4,&n);int font=luaL_optinteger(L,5,0);unsigned short c=lua_color(L,6);if(n>512||font<0||font>3)return luaL_error(L,"invalid label size/font");if(draw_admit(L))gfx_text_ellipsis(font,x,y,w,t,c);return 0;}
static int skin_text_width(lua_State*L){size_t n;const char*t=luaL_checklstring(L,1,&n);int font=luaL_optinteger(L,2,0);if(n>512||font<0||font>3)return luaL_error(L,"invalid text width request");lua_pushinteger(L,gfx_text_w(font,t));return 1;}
static int skin_cover(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544),w=dimension(L,3,0,480),h=dimension(L,4,0,272);if(!draw_admit(L)){lua_pushboolean(L,0);return 1;}int sw=0,sh=0;const unsigned short*art=api_art(&sw,&sh);if(art&&sw>0&&sh>0){if(g_page==SC_PLAYER&&store_disc_player()&&w==h&&w>=100&&w<=224){static float angle,last;float dt=g_app.time-last;last=g_app.time;if(!g_app.paused&&dt>=0&&dt<.1f)angle=fmodf(angle+dt*18.f,360.f);skin_disc_draw(x,y,w,angle);}else gfx_blit565_cover(x,y,w,h,art,sw,sh);lua_pushboolean(L,1);}else lua_pushboolean(L,0);return 1;}
static int skin_card_cover(lua_State*L){int index=luaL_checkinteger(L,1)-1,x=dimension(L,2,-480,960),y=dimension(L,3,-272,544),w=dimension(L,4,0,480),h=dimension(L,5,0,272);if(!draw_admit(L)){lua_pushboolean(L,0);return 1;}const CardList*cards=g_page==SC_SQUARE?api_discovery_cards():g_page==SC_SEARCH&&g_app.search_kind>0?api_search_entities():g_card_model;if(!cards||index<0||index>=cards->count){lua_pushboolean(L,0);return 1;}int slot=api_thumb_slot(cards->items[index].cover);const unsigned short*pixels=api_thumb_px(slot);if(pixels)gfx_blit565_cover(x,y,w,h,pixels,112,112);else api_request_thumbs();lua_pushboolean(L,pixels!=NULL);return 1;}
static int skin_overlay(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544),w=dimension(L,3,0,960),h=dimension(L,4,0,544);unsigned short c=lua_color(L,5);int alpha=luaL_optinteger(L,6,160);if(alpha<0||alpha>255)return luaL_error(L,"overlay alpha0..255");if(draw_admit(L))gfx_fill_rect_a(x,y,w,h,c,alpha);return 0;}
static int skin_tile_cover(lua_State*L){const char*source=luaL_checkstring(L,1);int index=luaL_checkinteger(L,2)-1,x=dimension(L,3,-480,960),y=dimension(L,4,-272,544),w=dimension(L,5,0,480),h=dimension(L,6,0,272);if(!draw_admit(L)){lua_pushboolean(L,0);return 1;}const char*url=NULL;
 if(!strcmp(source,"daily")){const Song*song=api_daily_preview();if(index==0&&song&&song->art[0])url=song->art;else api_request_daily_preview();}
 else if(!strcmp(source,"recommendations")){const CardList*cards=api_home_cards();if(index>=0&&index<cards->count&&cards->items[index].cover[0])url=cards->items[index].cover;}
 else return luaL_error(L,"unknown tile cover source");
 if(!url){lua_pushboolean(L,0);return 1;}int slot=api_thumb_slot(url);const unsigned short*pixels=api_thumb_px(slot);if(pixels){if(w>0&&h>0)gfx_blit565_cover(x,y,w,h,pixels,112,112);}else api_request_thumbs();lua_pushboolean(L,pixels!=NULL);return 1;}
static int skin_avatar(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544),w=dimension(L,3,0,480),h=dimension(L,4,0,272);if(!draw_admit(L)){lua_pushboolean(L,0);return 1;}const char*url=api_user_avatar();int slot=api_avatar_slot(url);const unsigned short*pixels=api_avatar_px(slot);if(pixels)gfx_blit565_cover(x,y,w,h,pixels,48,48);else if(url&&url[0])api_request_thumbs();lua_pushboolean(L,pixels!=NULL);return 1;}
static int skin_image(lua_State*L){const char*id=luaL_checkstring(L,1);int x=dimension(L,2,-480,960),y=dimension(L,3,-272,544),w=dimension(L,4,0,960),h=dimension(L,5,0,544),alpha=luaL_optinteger(L,6,255);if(alpha<0||alpha>255)return luaL_error(L,"image alpha0..255");int result=0;if(draw_admit(L)){result=skin_assets_image(id,x,y,w,h,alpha);vm(L)->begin+=skin_assets_decode_us();}lua_pushboolean(L,result>0);return 1;}
static int skin_sound(lua_State*L){const char*id=luaL_checkstring(L,1);lua_pushboolean(L,vm(L)->dry?0:skin_sound_play(id));return 1;}
static int skin_qr(lua_State*L){int x=dimension(L,1,0,480),y=dimension(L,2,0,272),size=dimension(L,3,32,224);if(!draw_admit(L)){lua_pushboolean(L,0);return 1;}int dim=0;const unsigned char*mods=api_qr_matrix(&dim);if(!mods||dim<=0||dim>57){lua_pushboolean(L,0);return 1;}int cell=size/(dim+8);if(cell<1){lua_pushboolean(L,0);return 1;}int real=(dim+8)*cell;gfx_fill_rect(x,y,real,real,gfx_rgb(255,255,255));for(int r=0;r<dim;++r)for(int c=0;c<dim;++c)if(mods[r*dim+c])gfx_fill_rect(x+(c+4)*cell,y+(r+4)*cell,cell,cell,gfx_rgb(0,0,0));lua_pushboolean(L,1);return 1;}
static int skin_slide(lua_State*L){int dx=dimension(L,1,-480,480),top=dimension(L,2,0,272),bottom=dimension(L,3,0,272);unsigned short c=lua_color(L,4);if(bottom<top)return luaL_error(L,"invalid slide area");if(draw_admit(L)&&dx){unsigned short*frame=gfx_backbuffer();if(frame)for(int y=top;y<bottom;++y){unsigned short*row=frame+y*FB_STRIDE;if(dx>0){memmove(row+dx,row,(480-dx)*2);for(int x=0;x<dx;++x)row[x]=c;}else{memmove(row,row-dx,(480+dx)*2);for(int x=480+dx;x<480;++x)row[x]=c;}}}return 0;}
static int skin_unfold(lua_State*L){lua_Number amount=luaL_checknumber(L,1);if(!isfinite((double)amount))return luaL_error(L,"invalid unfold progress");if(amount<0)amount=0;if(amount>1)amount=1;int top=dimension(L,2,0,272),bottom=dimension(L,3,0,272);unsigned short c=lua_color(L,4);if(bottom<top)return luaL_error(L,"invalid unfold region");if(draw_admit(L)&&amount<1){unsigned short*frame=gfx_backbuffer();int height=bottom-top,shown=(int)(height*amount);if(frame){for(int y=0;y<shown;++y){int sy=height?y*height/(shown?shown:1):0;memmove(frame+(top+y)*FB_STRIDE,frame+(top+sy)*FB_STRIDE,480*2);}for(int y=top+shown;y<bottom;++y)for(int x=0;x<480;++x)frame[y*FB_STRIDE+x]=c;}}return 0;}
static int skin_capture_page(lua_State*L){if(!draw_admit(L)){lua_pushboolean(L,1);return 1;}lua_pushboolean(L,skin_page_capture());return 1;}
static int skin_page_flip(lua_State*L){lua_Number progress=luaL_checknumber(L,1);int top=dimension(L,2,0,272),bottom=dimension(L,3,0,272);unsigned short c=lua_color(L,4);if(!isfinite((double)progress)||bottom<top)return luaL_error(L,"invalid page turn");if(draw_admit(L))skin_page_turn((float)progress,top,bottom,c);return 0;}
static int skin_release_page(lua_State*L){if(draw_admit(L))skin_page_release();return 0;}
static int skin_page_plane(lua_State*L){lua_Number progress=luaL_checknumber(L,1);int x=dimension(L,2,0,480),y=dimension(L,3,0,272),w=dimension(L,4,0,480),h=dimension(L,5,0,272);if(!isfinite((double)progress))return luaL_error(L,"invalid plane progress");if(draw_admit(L))skin_page_fold((float)progress,x,y,w,h);return 0;}
static int skin_begin_navigation(lua_State*L){SkinVM*v=vm(L);if(!v->dry)skin_transition_begin_wait(v->call_page,g_app.time);lua_pushboolean(L,1);return 1;}
static int skin_disc(lua_State*L){int x=dimension(L,1,0,480),y=dimension(L,2,0,272),size=dimension(L,3,32,224);lua_Number angle=luaL_checknumber(L,4);if(!isfinite((double)angle)||x+size>480||y+size>272)return luaL_error(L,"invalid disc geometry");if(!draw_admit(L)){lua_pushboolean(L,0);return 1;}if(g_page==SC_PLAYER&&!store_disc_player()){int sw=0,sh=0;const unsigned short*art=api_art(&sw,&sh);if(art&&sw>0&&sh>0)gfx_blit565_cover(x,y,size,size,art,sw,sh);lua_pushboolean(L,art!=NULL);return 1;}lua_pushboolean(L,skin_disc_draw(x,y,size,(float)angle));return 1;}
static int skin_gradient(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544),w=dimension(L,3,0,960),h=dimension(L,4,0,544);unsigned short top=lua_color(L,5),bottom=lua_color(L,6);int r=luaL_optinteger(L,7,0),horizontal=lua_toboolean(L,8);if(r<0||r>128)return luaL_error(L,"gradient radius limit");if(draw_admit(L)){if(horizontal)gfx_hgrad(x,y,w,h,top,bottom);else gfx_fill_round_grad(x,y,w,h,r,top,bottom);}return 0;}
static int skin_icon(lua_State*L){const char*name=luaL_checkstring(L,1);int x=dimension(L,2,-480,960),y=dimension(L,3,-272,544),size=dimension(L,4,4,128);unsigned short color=lua_color(L,5);int shown=0;if(draw_admit(L))shown=ui_skin_icon(name,x,y,size,color);lua_pushboolean(L,shown);return 1;}
static int skin_option_value(lua_State*L){const char*id=luaL_checkstring(L,1);int fallback=(int)luaL_optinteger(L,2,0);lua_pushinteger(L,vm(L)->dry?fallback:skin_options_value(id,fallback));return 1;}
static int skin_wave(lua_State*L){lua_Number t=luaL_checknumber(L,1);unsigned short c=lua_color(L,2);if(!isfinite((double)t))return luaL_error(L,"invalid wave time");if(draw_admit(L))skin_wave_draw((float)t,c);return 0;}
static int skin_music_fx(lua_State*L){int style=dimension(L,1,0,2);lua_Number t=luaL_checknumber(L,2);int level=dimension(L,3,0,100);if(!isfinite((double)t))return luaL_error(L,"invalid music effect time");if(draw_admit(L))skin_music_fx_draw(style,(float)t,level,g_vm.style.colors[6]);return 0;}
static int skin_lyric_text(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544),w=dimension(L,3,0,480);const char*t=luaL_checkstring(L,4);int font=dimension(L,5,0,3);unsigned short base=lua_color(L,6),active=lua_color(L,7);int index=dimension(L,8,1,MAX_LYRIC)-1,offset=(int)luaL_optinteger(L,9,0);if(strlen(t)>512||offset<0||offset>1024)return luaL_error(L,"invalid lyric fragment");if(draw_admit(L))lyric_draw_text(x,y,w,t,font,base,active,index,g_position,offset);return 0;}
static int skin_circle(lua_State*L){int x=dimension(L,1,-480,960),y=dimension(L,2,-272,544),r=dimension(L,3,0,480);unsigned short c=lua_color(L,4);if(draw_admit(L))gfx_circle(x,y,r,c);return 0;}
static int skin_clip(lua_State*L){int x=dimension(L,1,0,480),y=dimension(L,2,0,272),right=dimension(L,3,0,480),bottom=dimension(L,4,0,272);if(right<x||bottom<y)return luaL_error(L,"invalid clip rectangle");if(draw_admit(L))gfx_set_clip(x,y,right,bottom);return 0;}
static int skin_unclip(lua_State*L){if(draw_admit(L))gfx_reset_clip();return 0;}
static int skin_module(lua_State*L){const char*name=luaL_checkstring(L,1);int x=(int)luaL_optinteger(L,2,0),y=(int)luaL_optinteger(L,3,0),w=(int)luaL_optinteger(L,4,480),h=(int)luaL_optinteger(L,5,272);if(x<0||y<0||w<0||h<0||x+w>480||y+h>272)return luaL_error(L,"module viewport outside screen");if(!draw_admit(L))return 0;for(int i=0;i<g_module_count;++i)if(!strcmp(name,g_modules[i].name)){lua_pushboolean(L,g_modules[i].draw?g_modules[i].draw(x,y,w,h):0);return 1;}return luaL_error(L,"unknown UI module");}
static int action_allowed(const char*s){static const char*const allowed[]={"xmb_open","xmb_back","home","mine","search","songs","player","lyrics","downloads","cache","settings","skins","about","account","square","sources","comments","next","previous","pause","volume","seek","select_song","open_playlist","open_artist","search_query","download_song","like","refresh_skins","local","favorites","recent","liked_songs","my_playlists","setting","setting_adjust","mine_focus","mine_select","cache_select","cache_play","search_kind","search_edit","grid_select","category_select","search_open","toggle_translation","cancel_skin_load","daily"};for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i)if(!strcmp(s,allowed[i]))return 1;return 0;}
static int skin_action(lua_State*L){SkinVM*v=vm(L);const char*name=luaL_checkstring(L,1);const char*arg=luaL_optstring(L,2,"");if(!action_allowed(name)||strlen(arg)>=sizeof(g_argument))return luaL_error(L,"invalid app action or argument");if(v->dry){lua_pushboolean(L,1);return 1;}if(g_action[0]){lua_pushboolean(L,0);return 1;}snprintf(g_action,sizeof(g_action),"%s",name);snprintf(g_argument,sizeof(g_argument),"%s",arg);lua_pushboolean(L,1);return 1;}
static void field_s(lua_State*L,const char*name,const char*value){lua_pushstring(L,value?value:"");lua_setfield(L,-2,name);}
static void field_i(lua_State*L,const char*name,lua_Integer value){lua_pushinteger(L,value);lua_setfield(L,-2,name);}
static int g_cache_first;
void skin_runtime_set_cache_first(int first){g_cache_first=first<0?0:first;}
static void model(lua_State*L){int page=vm(L)->call_page;lua_newtable(L);field_i(L,"width",480);field_i(L,"height",272);field_i(L,"page",page);lua_pushnumber(L,vm(L)->call_dt);lua_setfield(L,-2,"frame_dt");field_i(L,"selected",g_app.list_sel+1);field_i(L,"home_selected",g_app.home_sel+1);field_i(L,"volume",audio_volume());field_i(L,"show_speed",store_show_speed());field_i(L,"show_memory",g_app.show_mem);field_i(L,"memory_used_kb",sysi_used_kb());field_i(L,"memory_free_kb",sysi_free_kb());field_i(L,"audio_state",g_audio_state);field_i(L,"position_ms",g_position);field_i(L,"duration_ms",g_duration);field_i(L,"cache_percent",audio_cache_pct());field_i(L,"buffered_ms",audio_buffered_ms());field_i(L,"buffering",g_audio_state==AUDIO_BUFFER);field_i(L,"lyric_translation",g_app.lyric_translation);field_i(L,"menu_open",g_app.menu_open);field_i(L,"menu_selected",g_app.menu_sel+1);field_i(L,"network_kbps",net_speed_kbps());field_i(L,"wlan_enabled",g_app.wlan_on);field_s(L,"wifi_name",g_app.wifi_name);field_i(L,"free_storage_mb",sysi_ms0_free_mb());field_i(L,"cache_bytes",cache_total_bytes());field_i(L,"pending_downloads",api_pending_download_count());field_i(L,"list_kind",g_list.kind);field_s(L,"list_title",g_list.title);field_s(L,"song",g_now_model?g_now_model->name:"");field_s(L,"artist",g_now_model?g_now_model->artist:"");char id[32];snprintf(id,sizeof(id),"%lld",g_now_model?g_now_model->id:0);field_s(L,"song_id",id);
 field_i(L,"paused",g_app.paused);field_i(L,"audio_level",audio_level());field_i(L,"disc_player",store_disc_player());field_i(L,"xmb_navigation",1);field_i(L,"liked",g_app.is_liked);field_i(L,"play_mode",g_app.play_mode);field_i(L,"online",g_app.online);field_i(L,"cache_count",cache_count());field_i(L,"local_count",local_count());field_i(L,"favorite_count",store_fav_count());field_i(L,"recent_count",store_recent_count());field_i(L,"download_count",dl_queue_count());field_i(L,"download_select_mode",g_app.download_select_mode);field_i(L,"download_selected_count",g_app.download_selected_count);field_i(L,"download_tab",g_app.dl_tab);field_i(L,"download_selected",g_app.dl_sel+1);field_i(L,"logged_in",api_logged_in());const Song*daily_preview=api_daily_preview();field_i(L,"daily_cover_available",daily_preview&&daily_preview->art[0]);field_s(L,"daily_song",daily_preview?daily_preview->name:"");field_s(L,"daily_artist",daily_preview?daily_preview->artist:"");field_i(L,"qr_state",api_qr_state());field_s(L,"login_name",api_login_name());field_s(L,"quality",g_app.quality==0?"标准128k":g_app.quality==1?"较高192k":"极高320k");field_s(L,"query",g_app.query);field_i(L,"query_len",g_app.query_len);field_i(L,"search_edit",g_app.kb_edit);field_i(L,"kb_row",g_app.kb_row);field_i(L,"kb_col",g_app.kb_col);field_i(L,"suggest_focused",g_app.sug_focus);field_i(L,"suggest_selected",g_app.sug_sel+1);lua_newtable(L);for(int i=0;i<api_suggest_count()&&i<12;++i){lua_pushstring(L,api_suggest(i));lua_rawseti(L,-2,i+1);}lua_setfield(L,-2,"suggestions");field_i(L,"ime_mode",ime_mode());field_i(L,"ime_active",ime_active()?1:0);field_s(L,"ime_pre",ime_active()?ime_preedit():"");field_i(L,"ime_selected",g_app.ime_sel+1);lua_newtable(L);{int n=ime_candidate_count();if(n>6)n=6;for(int i=0;i<n;++i){lua_pushstring(L,ime_candidate(i));lua_rawseti(L,-2,i+1);}}lua_setfield(L,-2,"ime_candidates");lua_newtable(L);{int n=search_hist_count();if(n>4)n=4;for(int i=0;i<n;++i){lua_pushstring(L,search_hist_at(i));lua_rawseti(L,-2,i+1);}}lua_setfield(L,-2,"search_history");lua_newtable(L);{int n=g_app.chips_n;if(n>APP_CHIP_MAX)n=APP_CHIP_MAX;for(int i=0;i<n;++i){lua_pushstring(L,g_app.chips[i]);lua_rawseti(L,-2,i+1);}}lua_setfield(L,-2,"search_chips");field_i(L,"chip_row",g_app.kb_row<=-1?(g_app.kb_row==-3?3:(g_app.kb_row==-2?2:1)):0);field_i(L,"chip_selected",g_app.chip_sel+1);field_i(L,"hist_selected",g_app.hist_sel+1);field_i(L,"ime_off",g_app.ime_off);lua_newtable(L);{for(int i=0;i<g_app.assoc_n&&i<APP_CHIP_MAX;++i){lua_pushstring(L,g_app.assoc[i]);lua_rawseti(L,-2,i+1);}}lua_setfield(L,-2,"assoc");field_i(L,"search_kind",g_app.search_kind);field_i(L,"search_state",g_app.search_state);field_s(L,"search_category",api_search_category_name(g_app.search_kind));field_i(L,"setting_selected",g_app.settings_sel+1);field_i(L,"mine_selected",g_app.mine_sel+1);field_i(L,"cache_selected",g_app.cache_sel+1);field_i(L,"cache_tab",g_app.cache_tab);field_i(L,"grid_selected",g_app.grid_page*12+g_app.grid_sel+1);field_i(L,"category_selected",g_app.cat_sel);field_i(L,"category_focused",g_app.cat_focus==0);if(g_app.time<g_status_sampled_at||g_app.time-g_status_sampled_at>=1.f){g_status_sampled_at=g_app.time;g_status_battery=scePowerGetBatteryLifePercent();ScePspDateTime clock;if(sceRtcGetCurrentClockLocalTime(&clock)>=0)snprintf(g_status_clock,sizeof(g_status_clock),"%02d:%02d",clock.hour,clock.minute);else g_status_clock[0]=0;}field_i(L,"battery",g_status_battery);field_s(L,"clock",g_status_clock);
 lua_newtable(L);const char*mine_labels[]={"我喜欢的音乐","最近播放","我的歌单","本地音乐","下载管理","缓存管理","设置","账号与登录","歌单广场","歌手榜","关于MELODY","收藏的歌单"};for(int i=0;i<12;++i){lua_newtable(L);field_i(L,"index",i+1);field_s(L,"label",mine_labels[i]);lua_rawseti(L,-2,i+1);}lua_setfield(L,-2,"mine");
 lua_newtable(L);if(g_setting_labels&&g_setting_values){int first=g_app.settings_sel>2?g_app.settings_sel-2:0,last=first+7;if(last>g_setting_count)last=g_setting_count;for(int i=first;i<last;++i){int id=melody_setting_id(i);lua_newtable(L);field_i(L,"index",i+1);field_s(L,"label",g_setting_labels[id]);field_s(L,"value",g_setting_values[id]);field_i(L,"group",g_setting_groups[id]);lua_rawseti(L,-2,i-first+1);}field_i(L,"total",g_setting_count);}lua_setfield(L,-2,"settings");
 if(page==SC_CACHE&&g_app.cache_tab==1){lua_newtable(L);int n=local_count(),first=g_app.local_sel>2?g_app.local_sel-2:0;for(int i=first;i<n&&i<first+20;++i){lua_newtable(L);field_i(L,"index",i+1);field_s(L,"name",local_name(i));field_s(L,"artist","本地MP3");lua_rawseti(L,-2,i-first+1);}field_i(L,"total",n);lua_setfield(L,-2,"local_files");field_i(L,"local_selected",g_app.local_sel+1);}
 if(page==SC_CACHE){const CacheEntry*cached=g_cache_model;int n=cached?g_cache_model_count:0;lua_newtable(L);int first=g_app.cache_sel-g_cache_first-2;if(first<0)first=0;for(int i=first;i<n&&i<first+20;++i){lua_newtable(L);char id[32];snprintf(id,sizeof(id),"%lld",cached[i].id);field_i(L,"index",g_cache_first+i+1);field_s(L,"id",id);field_s(L,"name",cached[i].name);field_s(L,"artist",cached[i].artist);field_i(L,"bytes",cached[i].bytes);field_i(L,"duration_ms",cached[i].dur_ms);lua_rawseti(L,-2,i-first+1);}field_i(L,"total",cache_count());lua_setfield(L,-2,"cache");}

 lua_newtable(L);if(g_list_model){int first=g_app.list_sel>3?g_app.list_sel-3:0;int last=first+24;if(last>g_list_model->count)last=g_list_model->count;for(int i=first;i<last;++i){const Song*s=&g_list_model->songs[i];lua_newtable(L);field_i(L,"index",i+1);char songid[32];snprintf(songid,sizeof(songid),"%lld",s->id);field_s(L,"id",songid);field_s(L,"name",s->name);field_s(L,"artist",s->artist);field_i(L,"playable",s->playable);field_i(L,"fee",s->fee);field_i(L,"download_marked",app_download_selected(s->id));lua_rawseti(L,-2,i-first+1);}field_i(L,"total",g_list_model->count);}lua_setfield(L,-2,"songs");
 const CardList*cards=page==SC_SQUARE?api_discovery_cards():(page==SC_SEARCH&&g_app.search_kind>0)?api_search_entities():g_card_model;
 lua_newtable(L);if(cards){int count=cards->count;if(count>24)count=24;for(int i=0;i<count;++i){const Card*c=&cards->items[i];lua_newtable(L);field_i(L,"index",i+1);char cardid[32];snprintf(cardid,sizeof(cardid),"%lld",c->id);field_s(L,"id",cardid);field_s(L,"name",c->name);field_s(L,"subtitle",c->sub);field_i(L,"tracks",c->tracks);field_i(L,"kind",c->kind);lua_rawseti(L,-2,i+1);}}lua_setfield(L,-2,"cards");
 if(page==SC_LYRICS||page==SC_PLAYER){const Lyric*ly=api_lyric();static float visual;static long long song;static int old_position;long long now_song=g_now_model?g_now_model->id:0;int cur=0;for(int i=0;i<ly->count;++i)if(ly->ms[i]<=g_position)cur=i;int gap=cur+1<ly->count?ly->ms[cur+1]-ly->ms[cur]:3000;if(song!=now_song||abs(g_position-old_position)>4000){visual=(float)cur;song=now_song;}visual=lyric_scroll_position(visual,(float)cur,vm(L)->drawing?vm(L)->call_dt:0,gap);old_position=g_position;field_i(L,"lyric_current",cur+1);lua_pushnumber(L,visual);lua_setfield(L,-2,"lyric_scroll");lua_newtable(L);int first=cur>3?cur-3:0;for(int i=first;i<ly->count&&i<first+12;++i){lua_newtable(L);field_i(L,"index",i+1);field_i(L,"ms",ly->ms[i]);field_s(L,"text",ly->text[i]);field_s(L,"translation",ly->translation[i]);field_i(L,"current",i==cur);lua_rawseti(L,-2,i-first+1);}lua_setfield(L,-2,"lyrics");}
 if(page==SC_COMMENTS){const CommentList*comments=api_comments();lua_newtable(L);for(int i=0;i<comments->count&&i<12;++i){const Comment*c=&comments->items[i];lua_newtable(L);char id[32];snprintf(id,sizeof(id),"%lld",c->id);field_s(L,"id",id);field_s(L,"user",c->user);field_s(L,"text",c->text);field_i(L,"likes",c->liked);field_i(L,"replies",c->reply_count);lua_rawseti(L,-2,i+1);}lua_setfield(L,-2,"comments");}
 if(page==SC_DOWNLOAD){static union{DlItem songs[DL_QUEUE_MAX];MvDlItem videos[MV_DL_MAX];}pending;lua_newtable(L);if(g_app.dl_tab){int count=mv_dl_snapshot(pending.videos,MV_DL_MAX);for(int i=0;i<count;++i){lua_newtable(L);field_s(L,"name",pending.videos[i].name);field_i(L,"index",i+1);field_i(L,"percent",pending.videos[i].pct);field_i(L,"state",pending.videos[i].state);field_i(L,"bytes",pending.videos[i].size);field_i(L,"total_bytes",pending.videos[i].size);lua_rawseti(L,-2,i+1);}}else{int count=dl_queue_snapshot(pending.songs,DL_QUEUE_MAX);for(int i=0;i<count;++i){lua_newtable(L);field_s(L,"name",pending.songs[i].song.name);field_i(L,"index",i+1);field_i(L,"percent",pending.songs[i].pct);field_i(L,"state",pending.songs[i].state);field_i(L,"total_bytes",pending.songs[i].total);field_i(L,"bytes",(int)((long long)pending.songs[i].total*pending.songs[i].pct/100));lua_rawseti(L,-2,i+1);}}lua_setfield(L,-2,"downloads");}
}
static int skin_model(lua_State*L){model(L);return 1;}
static const luaL_Reg ui_functions[]={{"lyric_text",skin_lyric_text},{"music_fx",skin_music_fx},{"wave",skin_wave},{"icon",skin_icon},{"gradient",skin_gradient},{"disc",skin_disc},{"begin_navigation",skin_begin_navigation},{"tile_cover",skin_tile_cover},{"avatar",skin_avatar},{"page_fold",skin_page_plane},{"capture_page",skin_capture_page},{"page_turn",skin_page_flip},{"release_page",skin_release_page},{"card_cover",skin_card_cover},{"overlay",skin_overlay},{"unfold",skin_unfold},{"slide",skin_slide},{"qr",skin_qr},{"image",skin_image},{"sound",skin_sound},{"line",skin_line},{"label",skin_label},{"text_width",skin_text_width},{"cover",skin_cover},{"rect",skin_rect},{"round",skin_round},{"text",skin_text},{"circle",skin_circle},{"clip",skin_clip},{"unclip",skin_unclip},{"module",skin_module},{NULL,NULL}};
static const luaL_Reg app_functions[]={{"option",skin_option_value},{"dispatch",skin_action},{"model",skin_model},{NULL,NULL}};
static void remove_global(lua_State*L,const char*name){lua_pushnil(L);lua_setglobal(L,name);}
static int skin_literal_find(lua_State*L){size_t length,needle_len;const char*text=luaL_checklstring(L,1,&length);const char*needle=luaL_checklstring(L,2,&needle_len);if(length>8192||needle_len>512)return luaL_error(L,"literal find size limit");lua_Integer first=luaL_optinteger(L,3,1);if(first<0)first=(lua_Integer)length+first+1;if(first<1)first=1;if(first>(lua_Integer)length+1){lua_pushnil(L);return 1;}size_t start=first-1;for(size_t i=start;i<=length&&needle_len<=length-i;++i){vm(L)->string_work+=(unsigned)(needle_len?needle_len:1);if(vm(L)->string_work>1048576u)return luaL_error(L,"skin string work budget exceeded");if(!memcmp(text+i,needle,needle_len)){lua_pushinteger(L,i+1);lua_pushinteger(L,i+needle_len);return 2;}}lua_pushnil(L);return 1;}
static int initialize_lua(lua_State*L){luaL_requiref(L,"_G",luaopen_base,1);lua_pop(L,1);luaL_requiref(L,LUA_TABLIBNAME,luaopen_table,1);lua_pop(L,1);luaL_requiref(L,LUA_STRLIBNAME,luaopen_string,1);lua_pop(L,1);luaL_requiref(L,LUA_MATHLIBNAME,luaopen_math,1);lua_pop(L,1);luaL_requiref(L,LUA_UTF8LIBNAME,luaopen_utf8,1);lua_pop(L,1);
 lua_getglobal(L,"string");const char*patterns[]={"match","gmatch","gsub"};for(int i=0;i<3;++i){lua_pushnil(L);lua_setfield(L,-2,patterns[i]);}lua_pushcfunction(L,skin_literal_find);lua_setfield(L,-2,"find");lua_pop(L,1);
 const char*denied[]={"dofile","loadfile","load","collectgarbage","pcall","xpcall","print","warn","setmetatable","getmetatable"};for(unsigned i=0;i<sizeof(denied)/sizeof(denied[0]);++i)remove_global(L,denied[i]);luaL_newlib(L,ui_functions);lua_setglobal(L,"ui");luaL_newlib(L,app_functions);lua_setglobal(L,"app");return 0;}
static int read_file(const char*root,const char*name,char**out,int max,char*err,int cap){
 char path[336];int n=snprintf(path,sizeof(path),"%s/%s",root,name);if(n<0||n>=(int)sizeof(path))return -1;
 int fd=sceIoOpen(path,PSP_O_RDONLY,0);if(fd<0){snprintf(err,cap,"missing %s",name);return -1;}long long size=sceIoLseek(fd,0,PSP_SEEK_END);
 if(size<1||size>max||sceIoLseek(fd,0,PSP_SEEK_SET)<0){sceIoClose(fd);snprintf(err,cap,"%s size limit",name);return -1;}
 char*buffer=malloc((size_t)size+1);if(!buffer){sceIoClose(fd);snprintf(err,cap,"skin text memory limit");return -1;}
 int got=0;while(got<size){int r=sceIoRead(fd,buffer+got,(int)size-got);if(r<=0)break;got+=r;}sceIoClose(fd);
 if(got!=size||memchr(buffer,0,(size_t)size)){free(buffer);snprintf(err,cap,"invalid text file");return -1;}buffer[size]=0;*out=buffer;return(int)size;
}
typedef struct{SkinStyle*style;SkinVM*owner;int schema,bad,script;}StyleCtx;
static void widget_field(StyleCtx*c,const char*path,int type,const char*str,long long number){
 const char*seg[8];int len[8],n=json_split_path(path,seg,len,8);if(n!=5||len[0]!=5||strncmp(seg[0],"pages",5)||len[2]!=7||strncmp(seg[2],"widgets",7))return;
 int page=-1;for(unsigned i=0;i<sizeof(page_names)/sizeof(page_names[0]);++i)if((int)strlen(page_names[i])==len[1]&&!strncmp(page_names[i],seg[1],len[1]))page=i;
 int index=atoi(seg[3]);if(page<0||index<0||index>=SKIN_WIDGET_MAX){c->bad=1;return;}
 SkinVM*v=c->owner;if(!v->widgets){v->widgets=calloc(SKIN_WIDGET_MAX,sizeof(*v->widgets));if(!v->widgets){c->bad=1;return;}}int slot=-1;/* Sparse page IDs share a single bounded widget table. */
 for(int i=0;i<v->widget_count;++i)if(v->widgets[i].page==page&&v->widgets[i].radius==index+1000){slot=i;break;}
 if(slot<0){if(v->widget_count>=SKIN_WIDGET_MAX){c->bad=1;return;}slot=v->widget_count++;v->widgets[slot].page=page;v->widgets[slot].radius=index+1000;v->widgets[slot].font=0;}
 SkinWidget*w=&v->widgets[slot];v->pages_mask|=1u<<page;const char*field=seg[4];int size=len[4];
#define WF(name) (size==(int)strlen(name)&&!strncmp(field,name,size))
 if(type==JSON_NUM){if(number<0||number>960){c->bad=1;return;}if(WF("x"))w->x=number;else if(WF("y"))w->y=number;else if(WF("width"))w->w=number;else if(WF("height"))w->h=number;else if(WF("font")){if(number>3)c->bad=1;else w->font=number;}}
 if(type==JSON_STR){char*target=NULL;size_t capacity=0;if(WF("type")){target=w->type;capacity=sizeof(w->type);}else if(WF("text")){target=w->text;capacity=sizeof(w->text);}else if(WF("bind")){target=w->binding;capacity=sizeof(w->binding);}else if(WF("action")){target=w->action;capacity=sizeof(w->action);}else if(WF("argument")){target=w->argument;capacity=sizeof(w->argument);}else if(WF("color")){if(!color(str,&w->color))c->bad=1;else w->set_color=1;}
  if(target){if(strlen(str)>=capacity)c->bad=1;else snprintf(target,capacity,"%s",str);}
 }
#undef WF
}
static void style_cb(void*ud,const char*path,int type,const char*str,long long num){StyleCtx*c=ud;widget_field(c,path,type,str,num);SkinStyle*s=c->style;if(type==JSON_TRUE||type==JSON_FALSE){int hide=type==JSON_FALSE;if(!strcmp(path,"chrome.mini_player"))s->hide_mini=hide;if(!strcmp(path,"chrome.status"))s->hide_status=hide;if(!strcmp(path,"chrome.transition"))s->hide_transition=hide;}if(!strcmp(path,"schema")&&type==JSON_NUM)c->schema=num;if(!strcmp(path,"script")&&type==JSON_TRUE)c->script=1;if(!strcmp(path,"dark")&&(type==JSON_TRUE||type==JSON_FALSE))s->dark=type==JSON_TRUE;
 if(type==JSON_STR){char*dst=NULL;int cap=0;if(!strcmp(path,"id")){dst=s->id;cap=sizeof(s->id);}else if(!strcmp(path,"name")){dst=s->name;cap=sizeof(s->name);}else if(!strcmp(path,"author")){dst=s->author;cap=sizeof(s->author);}else if(!strcmp(path,"version")){dst=s->version;cap=sizeof(s->version);}else if(!strcmp(path,"description")){dst=s->description;cap=sizeof(s->description);}if(dst){if(strlen(str)>=(unsigned)cap)c->bad=1;else snprintf(dst,cap,"%s",str);}if(!strcmp(path,"script")){if(strcmp(str,"skin.lua"))c->bad=1;else c->script=1;}
  if(!strncmp(path,"colors.",7)){int found=0;for(int i=0;i<SKIN_COLOR_COUNT;++i)if(!strcmp(path+7,color_names[i])){if(!color(str,&s->colors[i]))c->bad=1;else s->color_mask|=1u<<i;found=1;break;}if(!found)c->bad=1;}
 }
}
static int valid_id(const char*s){int n=strlen(s);if(n<1||n>32)return 0;for(int i=0;i<n;++i)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||(s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'))return 0;return 1;}
/* Saved roots follow the application when its installation directory moves. */
static int skin_relative_root(const char*path,char*root,int cap){
 const char*id=NULL;const char*scan=path;const char*part;
 if(!path||!root||cap<=0)return -1;
 if(!strncmp(path,"skins/",6))id=path+6;
 while((part=strstr(scan,"/skins/"))!=NULL){id=part+7;scan=id;}
 if(!id||!valid_id(id))return -1;
 int n=snprintf(root,cap,APP_DIR "skins/%s",id);
 return n>=0&&n<cap?0:-1;
}
static int vm_error(SkinVM*v,char*err,int cap){const char*message=v->L?lua_tostring(v->L,-1):NULL;snprintf(err,cap,"%s",message?message:"skin memory allocation failed");if(v->L){lua_close(v->L);v->L=NULL;}v->failed=1;return -1;}
static int invoke_callback(lua_State*L){SkinVM*v=vm(L);int page=v->call_page;float time=v->call_time,dt=v->call_dt;unsigned pressed=v->call_pressed,held=v->call_held;const char*event=v->event;lua_getglobal(L,"skin");if(!lua_istable(L,-1)){lua_pop(L,1);lua_pushboolean(L,0);return 1;}lua_getfield(L,-1,event);lua_remove(L,-2);if(!lua_isfunction(L,-1)){lua_pop(L,1);lua_pushboolean(L,0);return 1;}
 lua_pushstring(L,page>=0&&page<(int)(sizeof(page_names)/sizeof(page_names[0]))?page_names[page]:"unknown");lua_pushnumber(L,time);lua_pushnumber(L,dt);model(L);
 lua_newtable(L);field_i(L,"pressed",pressed);field_i(L,"held",held);field_i(L,"up",PSP_CTRL_UP);field_i(L,"down",PSP_CTRL_DOWN);field_i(L,"left",PSP_CTRL_LEFT);field_i(L,"right",PSP_CTRL_RIGHT);field_i(L,"confirm",PSP_CTRL_CIRCLE);field_i(L,"back",PSP_CTRL_CROSS);field_i(L,"square",PSP_CTRL_SQUARE);field_i(L,"triangle",PSP_CTRL_TRIANGLE);field_i(L,"select",PSP_CTRL_SELECT);field_i(L,"l",PSP_CTRL_LTRIGGER);field_i(L,"r",PSP_CTRL_RTRIGGER);
 lua_call(L,5,1);return 1;}
static int call_hook(SkinVM*v,const char*event,int page,float time,float dt,unsigned pressed,unsigned held,char*err,int cap){
 lua_State*L=v->L;if(!L)return 0;v->event=event;v->call_page=page;v->call_time=time;v->call_dt=dt;v->call_pressed=pressed;v->call_held=held;
 v->instructions=0;v->string_work=0;v->begin=sceKernelGetSystemTimeLow();v->calls=0;v->drawing=!strcmp(event,"draw");lua_pushcfunction(L,invoke_callback);lua_sethook(L,budget,LUA_MASKCOUNT,1000);int status=lua_pcall(L,0,1,0);lua_sethook(L,NULL,0,0);v->drawing=0;
 if(status!=LUA_OK){
  if(v->dry||status==LUA_ERRMEM){if(v==&g_vm){g_action[0]=0;g_notice=2;}return vm_error(v,err,cap);}
  const char*message=lua_tostring(L,-1);snprintf(err,cap,"%s",message?message:"skin callback failed");lua_pop(L,1);
  if(page>=0&&page<32)v->blocked_pages|=1u<<page;
  if(v==&g_vm){g_action[0]=0;g_notice=1;mlog("skin page disabled id=%s page=%d event=%s code=%d: %s",v->style.id,page,event,status,err);}
  return -1;
 }
 unsigned now=sceKernelGetSystemTimeLow(),elapsed=now-v->begin;
 if(!v->dry&&elapsed>SKIN_SLOW_FRAME_US){v->slow_frames++;if(!v->last_slow_log||now-v->last_slow_log>=5000000u){v->last_slow_log=now;mlog("skin slow callback id=%s page=%d event=%s wall_us=%u instructions=%u draws=%d total=%u (kept active)",v->style.id,page,event,elapsed,v->instructions,v->calls,v->slow_frames);}}
 int handled=lua_toboolean(L,-1);lua_pop(L,1);return handled;
}
static int prepare(SkinVM*v,const char*root,int dry,char*err,int cap){memset(v,0,sizeof(*v));v->dry=dry;if(strlen(root)>=sizeof(v->style.root)){snprintf(err,cap,"skin path too long");return -1;}snprintf(v->style.root,sizeof(v->style.root),"%s",root);char*text=NULL;int n=read_file(root,"skin.json",&text,SKIN_JSON_LIMIT,err,cap);if(n<0)return -1;StyleCtx ctx={&v->style,v,0,0,0};int parsed=json_walk(text,n,style_cb,&ctx);free(text);if(parsed<0||ctx.schema!=1||ctx.bad||!valid_id(v->style.id)||!v->style.name[0]){snprintf(err,cap,"invalid skin schema/id/name/colors");return -1;}for(int i=0;i<v->widget_count;++i){SkinWidget*w=&v->widgets[i];if(strcmp(w->type,"rect")&&strcmp(w->type,"text")&&strcmp(w->type,"module")){snprintf(err,cap,"unknown widget type");return -1;}if(w->x+w->w>960||w->y+w->h>544||(w->action[0]&&!action_allowed(w->action))){snprintf(err,cap,"invalid widget bounds/action");return -1;}}
 if(!skin_assets_probe(root,err,cap))return -1;
 v->script=ctx.script;if(!ctx.script)return 0;
 n=read_file(root,"skin.lua",&text,SKIN_SCRIPT_LIMIT,err,cap);if(n<0)return -1;v->L=lua_newstate(skin_alloc,v);if(!v->L){free(text);return vm_error(v,err,cap);}*(SkinVM**)lua_getextraspace(v->L)=v;lua_pushcfunction(v->L,initialize_lua);if(lua_pcall(v->L,0,0,0)!=LUA_OK){free(text);return vm_error(v,err,cap);}
 v->begin=sceKernelGetSystemTimeLow();lua_sethook(v->L,budget,LUA_MASKCOUNT,1000);int status=luaL_loadbufferx(v->L,text,n,"skin.lua","t");free(text);if(status==LUA_OK){v->instructions=0;v->begin=sceKernelGetSystemTimeLow();status=lua_pcall(v->L,0,1,0);}lua_sethook(v->L,NULL,0,0);if(status!=LUA_OK)return vm_error(v,err,cap);if(!lua_istable(v->L,-1)){lua_pop(v->L,1);snprintf(err,cap,"skin.lua must return a table");lua_close(v->L);v->L=NULL;return -1;}lua_setglobal(v->L,"skin");
 if(dry){for(int page=SC_HOME;page<SC_COUNT;++page){int result=call_hook(v,"draw",page,1,.016f,0,0,err,cap);if(result<0)return -1;}if(call_hook(v,"input",SC_HOME,1,.016f,PSP_CTRL_DOWN,0,err,cap)<0)return -1;}
 return 0;}
int skin_runtime_validate(const char*root,char*err,int cap){SkinVM candidate;int result=prepare(&candidate,root,1,err,cap);if(candidate.L)lua_close(candidate.L);free(candidate.widgets);return result;}
void skin_runtime_refresh_palette(void){
 SkinStyle*s=&g_vm.style;
 if(!strcmp(s->id,"xmb")){
  static const char*const tones[6][5]={{"#071A39","#18365B","#647FA4","#AFBED6","#D8EFFF"},{"#21133F","#382451","#806C9E","#C8B9DB","#E9D8FF"},{"#092E2A","#1B4740","#527F73","#AECDC1","#CAFFE3"},{"#42210E","#603A22","#9B7756","#DEC4A5","#FFE8BD"},{"#3D172C","#5B304A","#986080","#DAB5CD","#FFE0EE"},{"#152330","#304453","#6F899D","#BFCFD9","#FFFFFF"}};
  int selected=skin_options_value("color_style",0);if(selected<0||selected>5)selected=0;
  color(tones[selected][0],&s->colors[0]);s->colors[1]=s->colors[0];color(tones[selected][1],&s->colors[8]);color(tones[selected][2],&s->colors[5]);color(tones[selected][3],&s->colors[4]);color(tones[selected][4],&s->colors[6]);s->colors[2]=gfx_rgb(255,255,255);s->dark=1;return;
 }
 if(strcmp(s->id,"metro")&&strcmp(s->id,"metro_day"))return;
 static const char*const palettes[2][SKIN_COLOR_COUNT]={
  {"#080808","#080808","#FFFFFF","#BBBBBB","#969696","#3E3E3E","#0078D7","#0078D7","#171717","#FFFFFF","#000000","#54D49B","#F5A957","#292930","#000000"},
  {"#FFFFFF","#FFFFFF","#141414","#555555","#666666","#D2D2D2","#0078D7","#0078D7","#EFEFEF","#FFFFFF","#000000","#54D49B","#F5A957","#EFEFEF","#999999"}
 };
 int light=skin_options_value("color_mode",0)==1;
 for(int i=0;i<SKIN_COLOR_COUNT;++i)if(i!=6){color(palettes[light][i],&s->colors[i]);s->color_mask|=1u<<i;}
 s->dark=!light;
}
int skin_runtime_apply(const char*root,char*err,int cap){
 char canonical[256];int legacy_day=0;const char*tail=strrchr(root,'/');const char*back=strrchr(root,'\\');if(back&&(!tail||back>tail))tail=back;tail=tail?tail+1:root;
 if(!strcmp(tail,"metro_day")){int prefix=(int)(tail-root);int n=snprintf(canonical,sizeof(canonical),"%.*smetro",prefix,root);if(n<0||n>=(int)sizeof(canonical)){snprintf(err,cap,"skin path too long");return -1;}root=canonical;legacy_day=1;}
 SkinVM candidate;if(prepare(&candidate,root,0,err,cap)<0){if(candidate.L)lua_close(candidate.L);free(candidate.widgets);return -1;}/* allocator user-data must remain at a stable address after VM move */
 if(g_vm.L)lua_close(g_vm.L);free(g_vm.widgets);skin_assets_release();skin_sound_release();skin_disc_release();skin_wave_release();skin_transition_reset();g_vm=candidate;skin_assets_set_root(root);skin_sound_set_root(root);if(g_vm.L){lua_setallocf(g_vm.L,skin_alloc,&g_vm);*(SkinVM**)lua_getextraspace(g_vm.L)=&g_vm;}skin_options_load(root,g_vm.style.id);if(legacy_day&&!strcmp(g_vm.style.id,"metro")){for(int i=0;i<skin_options_count();++i){const SkinOption*o=skin_options_at(i);if(o&&!strcmp(o->id,"color_mode")){skin_options_set(i,1);break;}}}skin_runtime_refresh_palette();g_error[0]=0;g_action[0]=0;g_notice=0;mlog("skin applied id=%s version=%s script=%d",g_vm.style.id,g_vm.style.version,g_vm.script);if(skin_font_apply(root)>0)mlog("skin font: %d size(s), %d bytes",1,skin_font_bytes());else if(skin_font_last_error()[0])mlog("skin font skipped: %s",skin_font_last_error());
 int fd=sceIoOpen(SKIN_ACTIVE ".tmp",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);if(fd>=0){int size=strlen(root),written=0;while(written<size){int n=sceIoWrite(fd,root+written,size-written);if(n<=0)break;written+=n;}int closed=sceIoClose(fd);if(written==size&&closed>=0){sceIoRemove(SKIN_ACTIVE ".old");sceIoRename(SKIN_ACTIVE,SKIN_ACTIVE ".old");if(sceIoRename(SKIN_ACTIVE ".tmp",SKIN_ACTIVE)<0)sceIoRename(SKIN_ACTIVE ".old",SKIN_ACTIVE);else sceIoRemove(SKIN_ACTIVE ".old");}}
 return 0;}
void skin_runtime_reset(void){skin_options_reset();skin_wave_release();skin_disc_release();skin_transition_reset();skin_assets_release();skin_sound_release();skin_font_release();if(g_vm.L)lua_close(g_vm.L);free(g_vm.widgets);memset(&g_vm,0,sizeof(g_vm));g_error[0]=0;g_action[0]=0;g_notice=0;sceIoRemove(SKIN_ACTIVE);sceIoRemove(SKIN_ACTIVE ".tmp");sceIoRemove(SKIN_ACTIVE ".old");}
void skin_runtime_init(void){char path[256]={0};int fd=sceIoOpen(SKIN_ACTIVE,PSP_O_RDONLY,0);if(fd<0){sceIoRename(SKIN_ACTIVE ".old",SKIN_ACTIVE);fd=sceIoOpen(SKIN_ACTIVE,PSP_O_RDONLY,0);}if(fd<0)return;int n=sceIoRead(fd,path,sizeof(path)-1);sceIoClose(fd);if(n<=0)return;path[n]=0;char root[256];if(skin_relative_root(path,root,sizeof(root))<0)return;char err[192];if(skin_runtime_apply(root,err,sizeof(err))<0)snprintf(g_error,sizeof(g_error),"%s",err);}

const SkinStyle*skin_runtime_style(void){return &g_vm.style;}
const char*skin_runtime_error(void){return g_error;}
int skin_runtime_active(void){return g_vm.style.id[0]!=0;}
static int declarative_draw(int page){
 if(!g_vm.widgets||!(g_vm.pages_mask&(1u<<page)))return 0;unsigned short ink=g_vm.style.color_mask&4?g_vm.style.colors[2]:gfx_rgb(240,240,240);
 for(int i=0;i<g_vm.widget_count;++i){SkinWidget*w=&g_vm.widgets[i];if(w->page!=page)continue;unsigned short c=w->set_color?w->color:ink;
  if(!strcmp(w->type,"rect"))gfx_fill_rect(w->x,w->y,w->w,w->h,c);
  else if(!strcmp(w->type,"text")){const char*t=w->text;char dynamic[96];if(!strcmp(w->binding,"song.name"))t=g_now_model?g_now_model->name:"";else if(!strcmp(w->binding,"song.artist"))t=g_now_model?g_now_model->artist:"";else if(!strcmp(w->binding,"playback.position")){snprintf(dynamic,sizeof(dynamic),"%d:%02d",g_position/60000,g_position/1000%60);t=dynamic;}gfx_text_ellipsis(w->font,w->x,w->y,w->w?w->w:480-w->x,t,c);}
  else if(!strcmp(w->type,"module"))for(int j=0;j<g_module_count;++j)if(!strcmp(w->text,g_modules[j].name)&&g_modules[j].draw){g_modules[j].draw(w->x,w->y,w->w,w->h);break;}
 }
 int focus=0;for(int i=0;i<g_vm.widget_count;++i){SkinWidget*w=&g_vm.widgets[i];if(w->page!=page||!w->action[0])continue;if(focus++==g_vm.focused_widget){gfx_fill_rect(w->x,w->y+(w->h?w->h:18),w->w?w->w:80,2,g_vm.style.colors[6]);break;}}
 return 1;
}
int skin_runtime_draw(int page,float time,float dt){skin_assets_begin_frame();if(g_vm.failed||(page>=0&&page<32&&(g_vm.blocked_pages&(1u<<page))))return 0;g_page=page;int base=declarative_draw(page);if(!g_vm.L)return base;int result=call_hook(&g_vm,"draw",page,time,dt,0,0,g_error,sizeof(g_error));gfx_reset_clip();if(result<0){mlog("skin draw stopped: %s",g_error);return 0;}return result||base;}
static int declarative_input(int page,unsigned pressed){
 if(!g_vm.widgets)return 0;int slots[SKIN_WIDGET_MAX],count=0;for(int i=0;i<g_vm.widget_count;++i)if(g_vm.widgets[i].page==page&&g_vm.widgets[i].action[0])slots[count++]=i;if(!count)return 0;
 if(g_vm.focus_page!=page){g_vm.focus_page=page;g_vm.focused_widget=0;}if(g_vm.focused_widget>=count)g_vm.focused_widget=0;
 if(pressed&(PSP_CTRL_UP|PSP_CTRL_LEFT))g_vm.focused_widget=(g_vm.focused_widget+count-1)%count;
 if(pressed&(PSP_CTRL_DOWN|PSP_CTRL_RIGHT))g_vm.focused_widget=(g_vm.focused_widget+1)%count;
 if((pressed&PSP_CTRL_CIRCLE)&&!g_action[0]){SkinWidget*w=&g_vm.widgets[slots[g_vm.focused_widget]];snprintf(g_action,sizeof(g_action),"%s",w->action);snprintf(g_argument,sizeof(g_argument),"%s",w->argument);}
 return(pressed&(PSP_CTRL_UP|PSP_CTRL_DOWN|PSP_CTRL_LEFT|PSP_CTRL_RIGHT|PSP_CTRL_CIRCLE))!=0;
}
int skin_runtime_input(int page,unsigned pressed,unsigned held,float dt){if(g_vm.failed||(page>=0&&page<32&&(g_vm.blocked_pages&(1u<<page))))return 0;g_page=page;int result=g_vm.L?call_hook(&g_vm,"input",page,g_app.time,dt,pressed,held,g_error,sizeof(g_error)):0;if(result<0)return 0;if(result>0)return 1;return declarative_input(page,pressed);}
int skin_runtime_action(char*out,int n,char*arg,int cap){if(!g_action[0])return 0;snprintf(out,n,"%s",g_action);snprintf(arg,cap,"%s",g_argument);g_action[0]=0;return 1;}
void skin_runtime_set_model(int page,const SongList*list,const CardList*cards){g_page=page;g_list_model=list;g_card_model=cards;}
void skin_runtime_set_playback(const Song*song,int state,int position,int duration){g_now_model=song;g_audio_state=state;g_position=position;g_duration=duration;}
int skin_runtime_register(const SkinModule*modules,int count){if(count<0||count>40)return -1;g_modules=modules;g_module_count=count;return 0;}
void skin_runtime_suspend(void){skin_wave_release();skin_disc_release();skin_transition_reset();mlog("skin resources: image cache before MV=%u",skin_assets_memory_bytes());skin_assets_release();skin_sound_release();if(g_vm.L){mlog("skin: suspend id=%s heap=%u",g_vm.style.id,(unsigned)g_vm.bytes);lua_close(g_vm.L);g_vm.L=NULL;}free(g_vm.widgets);g_vm.widgets=NULL;g_list_model=NULL;g_card_model=NULL;g_cache_model=NULL;g_cache_model_count=0;}
void skin_runtime_resume(void){if(!g_vm.style.root[0]||(!g_vm.script&&!g_vm.pages_mask)||g_vm.L)return;char path[256],err[192];snprintf(path,sizeof(path),"%s",g_vm.style.root);if(skin_runtime_apply(path,err,sizeof(err))<0)snprintf(g_error,sizeof(g_error),"%s",err);else mlog("skin: resumed id=%s",g_vm.style.id);}

void skin_runtime_set_settings(const char*const*labels,const int*groups,const char values[][96],int count){g_setting_labels=labels;g_setting_groups=groups;g_setting_values=values;g_setting_count=count;}

int skin_runtime_take_notice(void){int notice=g_notice;g_notice=0;return notice;}

void skin_runtime_set_cache(const CacheEntry*items,int count){g_cache_model=items;g_cache_model_count=count;}
