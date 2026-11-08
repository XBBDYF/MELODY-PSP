"""Run actual UI drawing/worker dispatch to isolate homepage cost during download."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/update-foreground';O.mkdir(exist_ok=True)
u=(R/'src/ui.c').read_text(encoding='utf-8');a=u.index('void ui_draw(void)');b=u.index('\n/* Pending MV',a);body=u[a:b]
prefix=r'''
#include <stdio.h>
#include <assert.h>
#define SC_BOOT 0
#define SC_WIFI 1
#define SC_HOME 2
#define SC_LIST 3
#define SC_PLAYER 4
#define SC_LYRICS 5
#define SC_SEARCH 6
#define SC_DOWNLOAD 7
#define SC_CACHE 8
#define SC_SETTINGS 9
#define SC_HELP 10
#define SC_LOGIN 11
#define SC_GRID 12
#define SC_SQUARE 13
#define SC_COMMENTS 14
#define SC_MINE 15
#define SC_ALT 16
#define SC_MV 17
#define SC_SKINS 18
#define SCR_W 480
#define SCR_H 272
#define C_BLACK 0
#define C_WHITE 1
#define C_PANEL 2
static struct{float time,enter,spinner;int screen,busy,busy_x,confirm_clear,upd_dlg,has_now,menu_open,connecting;int now;}g_app;
static int g_list;static int g_cache_view[1],g_cache_view_n;
static int g_cache_view_first;
static void skin_runtime_set_cache_first(int first){(void)first;}
static void skin_runtime_set_cache(const int*items,int n){(void)items;(void)n;}
static int g_skin_modules[1];
#define MELODY_SETTINGS_COUNT 22
static const struct{int group;const char*label;}g_set_defs[MELODY_SETTINGS_COUNT]={{0,"quality"}};
static void settings_value(int i,char*out,int n){(void)i;(void)n;out[0]=0;}
static int skin_runtime_active(void){return 0;}
static void skin_runtime_set_settings(const char*const*labels,const int*groups,const char values[][96],int n){(void)labels;(void)groups;(void)values;(void)n;}

static int g_dark,state=4,home_draws,settings_draws,views,dialogs,status_draws;
static int api_update_state(void){return state;}
static float clamp01f(float x){return x<0?0:x>1?1:x;}
static void metro_loading_line(float t){(void)t;}
static void skin_options_dialog_draw(void){}
static int skin_options_dialog_active(void){return 0;}
static int skin_transition_active(void){return 0;}
static void skin_transition_request_section(int p,int a,int b,float t){(void)p;(void)a;(void)b;(void)t;}
static void skin_transition_draw(int page,float t){(void)page;(void)t;}
static int skin_transition_waiting(void){return 0;}
static int is_metro_style(void){return 0;}
static void gfx_reset_clip(void){}
static void player_menu_draw(void){}
static void views_refresh(void){views++;}
static void ui_free_skin_preview(void){}
static void screen_boot(float t){(void)t;}
#define SCREEN(name) static void name(float t,float dt){(void)t;(void)dt;}
SCREEN(screen_wifi) SCREEN(screen_list) SCREEN(screen_player) SCREEN(screen_lyrics) SCREEN(screen_search) SCREEN(screen_download) SCREEN(screen_cache) SCREEN(screen_help) SCREEN(screen_login) SCREEN(screen_grid) SCREEN(screen_square) SCREEN(screen_comments) SCREEN(screen_mine) SCREEN(screen_alt) SCREEN(screen_skins)
static void skin_runtime_register(const int*m,int n){(void)m;(void)n;}
static void skin_runtime_set_model(int p,const int*l,const int*c){(void)p;(void)l;(void)c;}
static void skin_runtime_set_playback(const int*s,int st,int p,int d){(void)s;(void)st;(void)p;(void)d;}
static const struct SkinStyleStub{int hide_mini,hide_status,hide_transition;}*skin_runtime_style(void){static struct SkinStyleStub style;return &style;}
static int skin_runtime_draw(int p,float t,float d){(void)p;(void)t;(void)d;return 0;}
static int audio_state(void){return 0;}static int audio_pos_ms(void){return 0;}static int audio_dur_ms(void){return 0;}
static const int*api_home_cards(void){return NULL;}static const int*api_cards(void){return NULL;}
static void screen_home(float t,float dt){(void)t;(void)dt;home_draws++;}
static void screen_settings(float t,float dt){(void)t;(void)dt;settings_draws++;}
static void gfx_clear(int c){(void)c;}
static void player_bar_draw(void){}
static void gfx_fill_rect_a(int x,int y,int w,int h,int c,int a){(void)x;(void)y;(void)w;(void)h;(void)c;(void)a;}
static void spinner_draw(int x,int y,float t){(void)x;(void)y;(void)t;}
static void screen_net_speed(void){status_draws++;}
static void export_progress_draw(void){}
static void toast_draw(void){}
static void confirm_draw(void){}
static void upd_dialog_draw(void){dialogs++;}
'''
tail=r'''
int main(void){g_app.screen=SC_HOME;g_app.upd_dlg=1;g_app.enter=1;g_app.time=1;ui_draw();assert(dialogs==1&&status_draws==1);printf("active update home draws=%d views=%d\n",home_draws,views);fflush(stdout);assert(home_draws==0&&views==0);g_app.screen=SC_SETTINGS;ui_draw();assert(settings_draws==0&&views==0&&dialogs==2);state=7;g_app.screen=SC_HOME;ui_draw();assert(home_draws==0);state=3;ui_draw();assert(home_draws==1&&views==1);g_app.upd_dlg=0;state=4;ui_draw();assert(home_draws==2);puts("PASS real UI active/queued download uses same lightweight progress view from Home/Settings; normal page preserved outside modal");return 0;}
'''
(O/'ui.c').write_text(prefix+body+tail,encoding='utf-8');exe=O/'ui.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',O/'ui.c','-o',exe],check=True);ui_result=subprocess.run([exe]);
s=(R/'src/api.c').read_text(encoding='utf-8');a=s.index('        case JOB_UPDATE_DL:');b=s.index('\n',a);dispatch=s[a:b]
helper=''
if 'static void job_update_download_foreground('in s:
 a=s.index('static void job_update_download_foreground(');b=s.index('\nstatic ',a+20);helper=s[a:b]
worker_priority=int(__import__('re').search(r'#define WORKER_PRIO\s+(0x[0-9a-fA-F]+)',s)[1],16)
prefix=r'''
#include <stdio.h>
#include <assert.h>
#define JOB_UPDATE_DL 1
static int priority,observed,failed,changes;
static int sceKernelGetThreadCurrentPriority(void){return priority;}
static int sceKernelChangeThreadPriority(int thread,int p){(void)thread;if(failed)return -1;priority=p;changes++;return 0;}
static void job_update_download(void){observed=priority;}
#define mlog(...) ((void)0)
'''
tail='''\nstatic void dispatch(void){switch(1){\n'''+dispatch+'''\n}}\nint main(void){priority='''+str(worker_priority)+r''';dispatch();printf("actual download dispatch priority=%d UI priority=32\n",observed);fflush(stdout);assert(observed<32&&observed>21);assert(priority=='''+str(worker_priority)+r'''&&changes==2);changes=0;failed=1;dispatch();assert(priority=='''+str(worker_priority)+r'''&&observed==priority&&changes==0);puts("PASS download alone preempts heavy UI, audio stays higher, worker priority restored, failed boost falls back");return 0;}
'''
(O/'worker.c').write_text(prefix+helper+tail,encoding='utf-8');exe=O/'worker.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',O/'worker.c','-o',exe],check=True);worker_result=subprocess.run([exe]);assert ui_result.returncode==0 and worker_result.returncode==0
