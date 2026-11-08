"""Download SELECT must play its selected song, independent of browsing rows.

Compiles production download controls and playback preparation; the API request
is the boundary to the asynchronous worker. Replacing SELECT with cache navigation
must fail the selected identity assertions.
"""
from pathlib import Path
import subprocess
import tempfile

R = Path(__file__).resolve().parents[1]
s = (R / 'src/main.c').read_text(encoding='utf-8')
def function(signature):
    start = s.index(signature)
    return s[start:s.index('\n}', start) + 2]
start = s.index('        DlItem items[DL_QUEUE_MAX];', s.index('static void input_download('))
branch = s[start:s.index('\n    }\n}', start)]
helper = function('static void select_download_play(') if 'static void select_download_play(' in s else ''
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DL_QUEUE_MAX 32
#define DL_PAUSED 4
#define PLAYBACK_ONLINE 1
#define SC_PLAYER 3
#define SC_CACHE 5
#define SC_MINE 6
#define PSP_CTRL_UP 1
#define PSP_CTRL_DOWN 2
#define PSP_CTRL_CIRCLE 4
#define PSP_CTRL_TRIANGLE 8
#define PSP_CTRL_SQUARE 16
#define PSP_CTRL_CROSS 32
#define PSP_CTRL_SELECT 64
typedef struct {long long id;char name[64],artist[64],album[64];} Song;
typedef struct {int count;Song songs[32];} SongList;
typedef struct {long long id;int state;Song song;} DlItem;
static struct {int playing_index,is_liked,has_now,paused,lyrics_open,lyric_scroll,marquee,busy,screen,play_from,play_from_tab,cache_tab,cache_sel,dl_sel,confirm_clear;Song now;} g_app;
static struct {int source,index,count;union {SongList online;}data;} g_playback;
static SongList g_list;
static int preserve_play_page,g_play_fail,g_retry_gave_up,g_retry_n,g_retry_waiting,g_retry_t;
static long long g_dl_focus_song;
static DlItem fixture[3];static int fixture_n,requests,pauses,resumes,request_index;static Song requested;
static void mlog(const char *fmt,...){(void)fmt;}
static void store_push_recent(const Song*s){(void)s;}
static void api_request_play(int index,const Song*s){requests++;requested=*s;request_index=index;}
static void set_screen(int screen){g_app.screen=screen;}
static void ui_toast(const char*s){assert(s&&s[0]);}
static int btn_repeat(unsigned p,unsigned h,float d,unsigned k){(void)h;(void)d;return !!(p&k);}
static int dl_queue_snapshot(DlItem*out,int cap){assert(cap>=fixture_n);memcpy(out,fixture,sizeof(DlItem)*fixture_n);return fixture_n;}
static int dl_queue_pause(int i){assert(i==1);pauses++;return 0;}
static int dl_queue_resume(int i){assert(i==1);resumes++;return 0;}
static void dl_queue_cancel(int i){(void)i;}
static int dl_queue_count(void){return fixture_n;}
'''
code += function('static void play_online_context_index(') + '\n' + helper
code += '\nstatic void input(unsigned pressed){unsigned held=0;float dt=0;\n' + branch + '\n}\n'
code += r'''
int main(void){fixture_n=3;for(int i=0;i<3;i++){fixture[i].id=101+i;fixture[i].song.id=101+i;}fixture[1].state=DL_PAUSED;strcpy(fixture[1].song.name,"selected partial");g_app.dl_sel=1;g_list.count=1;g_list.songs[0].id=999;g_app.screen=7;input(PSP_CTRL_SELECT);assert(requests==1);assert(requested.id==102&&request_index==1);assert(!strcmp(requested.name,"selected partial"));assert(g_app.now.id==102&&g_app.has_now&&g_app.busy&&!g_app.paused&&g_app.screen==SC_PLAYER);assert(g_playback.count==3&&g_playback.data.online.songs[2].id==103);assert(g_list.count==1&&g_list.songs[0].id==999);fixture[1].song.id=777;assert(g_playback.data.online.songs[1].id==102);assert(!pauses&&!resumes);input(PSP_CTRL_CIRCLE);assert(resumes==1&&requests==1);fixture[1].state=1;input(PSP_CTRL_CIRCLE);assert(pauses==1);fixture_n=0;input(PSP_CTRL_SELECT);assert(requests==1);fixture_n=3;g_app.dl_sel=3;input(PSP_CTRL_SELECT);assert(requests==1);puts("PASS production download SELECT selected identity, playback context, empty/stale selection, separate pause/resume");}
'''
with tempfile.TemporaryDirectory(prefix='download-select-') as tmp:
    c, exe = Path(tmp)/'test.c', Path(tmp)/'test.exe'
    c.write_text(code, encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-function',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)], check=True)
