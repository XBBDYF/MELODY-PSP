#ifndef MELODY_SKIN_RUNTIME_H
#define MELODY_SKIN_RUNTIME_H
#include "app.h"
#include "cache.h"
#define SKIN_COLOR_COUNT 15
#define SKIN_WIDGET_MAX 96
#define SKIN_PAGE_COUNT SC_COUNT
/* Page renderers are feature modules; downloaded skin code only chooses/draws/views them. */
typedef struct{unsigned short colors[SKIN_COLOR_COUNT];unsigned color_mask;int dark,hide_mini,hide_status,hide_transition,power_profile;char id[33],name[64],author[64],version[24],description[192],root[256];}SkinStyle;
typedef struct{const char*name;int (*draw)(int x,int y,int w,int h);int (*input)(unsigned pressed,unsigned held,float dt);}SkinModule;
void skin_runtime_init(void);
int skin_runtime_validate(const char*root,char*err,int errlen);
int skin_runtime_apply(const char*root,char*err,int errlen);
void skin_runtime_reset(void);
const SkinStyle*skin_runtime_style(void);
/* Refresh Metro colors in place after preference changes, without reloading Lua. */
void skin_runtime_refresh_palette(void);
const char*skin_runtime_error(void);
int skin_runtime_take_notice(void); /*1 page fallback,2 fatal VM memory failure*/
int skin_runtime_active(void);
/* Called on main/UI thread. Return1 only when skin supplied this whole page. */
int skin_runtime_draw(int page,float time,float dt);
int skin_runtime_input(int page,unsigned pressed,unsigned held,float dt);
/* Async-safe action bridge: main loop polls and executes feature commands. */
int skin_runtime_action(char*out,int outlen,char*arg,int arglen);
void skin_runtime_set_model(int page,const SongList*list,const CardList*cards);
void skin_runtime_set_cache(const CacheEntry*items,int count);
void skin_runtime_set_playback(const Song*song,int state,int position,int duration);
void skin_runtime_set_settings(const char*const*labels,const int*groups,const char values[][96],int count);
int skin_runtime_register(const SkinModule*modules,int count);
void skin_runtime_suspend(void);
void skin_runtime_resume(void);
void skin_runtime_set_cache_first(int first);
#endif
