"""Compile actual main.c playback functions on the host; no PSP or network."""
from pathlib import Path
import subprocess
R = Path(__file__).resolve().parents[1]
O = R / '.tmp/playback-context'
O.mkdir(parents=True, exist_ok=True)
s = (R / 'src/main.c').read_text(encoding='utf-8')
assert 'static PlaybackContext *g_playback_storage;' in s, 'playback snapshot must leave static BSS for the fixed root heap'
startup = s[s.index('int app_main(void)'):]
assert startup.index('log_open();') < startup.index('playback_context_init()') < startup.index('api_init();')
assert 'if (playback_context_init() < 0)' in startup
assert 'log_close();' in startup[startup.index('if (playback_context_init() < 0)'):startup.index('update_apply_startup();')]
a = s.index('static NextPlan g_next_plan;')
b = s.index('/* 缓存目录里有 mp3', a)
c = s.index('static void play_local_file(')
# Include the path helper when it is extracted from the original entry point.
p = s.find('static void play_local_path(')
if p >= 0 and p < c:
    c = p
d = s.index('static void download_current(', c)
eof_start = s.index('if (audio_state() == AUDIO_END && g_app.has_now)')
eof_end = s.index('if (audio_state() == AUDIO_ERROR', eof_start)
eof = s[eof_start:eof_end]
header = '#include "playback_context.h"\n' if (R/'src/playback_context.h').exists() else ''
code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "app.h"
#include "cache.h"
#include "local.h"
#include "next_plan.h"
'''+header+r'''
#undef APP_DIR
#define APP_DIR ""
AppState g_app;
SongList g_list;
static int g_play_fail,g_retry_gave_up,g_retry_n,g_retry_waiting,preserve_play_page,g_auto_skip;
static float g_retry_t;
static int request_count,prefetch_count;
static long long requested_id;
static char played_path[256];
static CacheEntry entries[CACHE_MAX];static int entry_count;
static const char *paths[3]={"ms0:/MUSIC/a.mp3","ms0:/MUSIC/b.mp3","ms0:/MUSIC/c.mp3"};
#define AUDIO_PLAY 1
#define AUDIO_END 2
static int state=AUDIO_PLAY,g_end_done,seek_count;
static int audio_state(void){return state;}
static int audio_pos_ms(void){return 1000;}
static int audio_replay_current(const Song *song){assert(song==&g_app.now);seek_count++;state=AUDIO_PLAY;return 1;}
static int store_prefetch_count(void){return 5;}
static int dl_has_song(long long id){(void)id;return 0;}
static int api_download_pending(long long id){(void)id;return 0;}
static void api_request_prefetch(const Song*s){prefetch_count++;requested_id=s->id;}
static void mlog(const char*fmt,...){(void)fmt;}
static void store_push_recent(const Song*s){(void)s;}
static void set_screen(int sc){g_app.screen=sc;}
static void api_request_play(int i,const Song*s){(void)i;request_count++;requested_id=s->id;}
int cache_snapshot(CacheEntry*out,int max){int n=entry_count<max?entry_count:max;memcpy(out,entries,n*sizeof(*out));return n;}
int cache_snapshot_ids(const char*path){FILE*f=fopen(path,"wb");assert(f);for(int i=0;i<entry_count;i++)assert(fwrite(&entries[i].id,8,1,f)==1);assert(!fclose(f));return entry_count;}
int cache_count(void){return entry_count;}
const CacheEntry*cache_get(int i){return i>=0&&i<entry_count?&entries[i]:NULL;}
const CacheEntry*cache_find(long long id){for(int i=0;i<entry_count;i++)if(entries[i].id==id)return &entries[i];return NULL;}
int cache_has(long long id){for(int i=0;i<entry_count;i++)if(entries[i].id==id)return 1;return 0;}
int local_count(void){return 3;}
const char*local_path_of(int i){return i>=0&&i<3?paths[i]:NULL;}
const char*local_name(int i){return strrchr(paths[i],'/')+1;}
static int api_local_meta(const char*p,Song*s){(void)p;(void)s;return 0;}
static int audio_play_file(const char*p,Song*s){(void)s;snprintf(played_path,sizeof(played_path),"%s",p);return 1;}
static const char*audio_error(void){return "error";}
static void ui_toast(const char*s){(void)s;}
static int api_local_art(const char*p){(void)p;return 0;}
static long long mp3_id_from_path(const char*p){(void)p;return 0;}
static void api_request_local_id(long long id){(void)id;}
static int mp3_dur_ms(const char*p,void*k){(void)p;(void)k;return 1000;}
static int audio_dur_ms(void){return 1000;}
static void api_request_local_meta(const char*n,const char*a,int d){(void)n;(void)a;(void)d;}
static void api_clear_lyric(void){}
static int allocation_fail,allocation_calls;
static void *test_calloc(size_t count,size_t bytes){allocation_calls++;return allocation_fail?NULL:calloc(count,bytes);}
#define calloc test_calloc
'''+s[a:b]+s[c:d]+'\nstatic void run_eof(void){'+eof+'}\n'+r'''
static void seed_cache(void){entry_count=3;for(int i=0;i<3;i++){entries[i].id=101+i;snprintf(entries[i].name,sizeof(entries[i].name),"cached-%d",i);}}
static void browse(void){g_list.count=2;g_list.songs[0].id=900;g_list.songs[1].id=901;}
int main(void){
 allocation_fail=1;assert(playback_context_init()<0&&!g_playback_storage&&allocation_calls==1);
 allocation_fail=0;assert(playback_context_init()==0&&g_playback_storage&&allocation_calls==2);
 assert(g_playback.source==PLAYBACK_NONE&&g_playback.count==0&&g_playback.index==0);
 assert(playback_context_init()==0&&allocation_calls==2); /* one allocation for process lifetime */
 puts("PASS actual early playback initializer: allocation failure stays NULL, success zeroes full context, repeat initialization reuses storage");
 seed_cache();g_app.play_mode=0;play_cache_entry(0);assert(requested_id==101);browse();
 play_next(1);assert(requested_id==102);assert(!strcmp(g_app.now.name,"cached-1"));
 /* Cache sorting/reindexing must resolve the next stable ID, not the old row. */
 CacheEntry swap=entries[0];entries[0]=entries[2];entries[2]=swap;
 play_next(1);assert(requested_id==103);assert(!strcmp(g_app.now.name,"cached-2"));
 play_prev();assert(requested_id==102);play_prev();assert(requested_id==101);play_prev();assert(requested_id==103);
 int calls=request_count;play_next(1);assert(request_count==calls); /* sequential end */
 play_cache_entry(0);entries[1]=entries[0];entry_count=1;calls=request_count;play_prev();assert(request_count==calls); /* missing ID never becomes online */
 seed_cache();play_cache_entry(0);g_app.play_mode=1;play_next(1);assert(requested_id==102||requested_id==103);
 g_app.play_mode=2;play_next(1);assert(requested_id>=101&&requested_id<=103); /* manual still advances */
 seed_cache();g_app.play_mode=0;play_cache_entry(0);browse();g_app.screen=SC_LIST;state=AUDIO_END;g_end_done=0;run_eof();assert(requested_id==102&&g_app.screen==SC_LIST);calls=request_count;run_eof();assert(request_count==calls);
 g_app.play_mode=2;g_end_done=0;run_eof();assert(seek_count==1&&request_count==calls&&!g_app.paused);state=AUDIO_PLAY;
 g_app.play_mode=0;play_local_file(0);assert(!strcmp(played_path,"ms0:/MUSIC/a.mp3"));browse();
 paths[0]="ms0:/MUSIC/new.mp3";paths[1]="ms0:/MUSIC/reordered.mp3";
 state=AUDIO_END;g_end_done=0;run_eof();assert(!strcmp(played_path,"ms0:/MUSIC/b.mp3"));state=AUDIO_PLAY;play_prev();assert(!strcmp(played_path,"ms0:/MUSIC/a.mp3"));play_prev();assert(!strcmp(played_path,"ms0:/MUSIC/c.mp3"));
 g_list.count=3;for(int i=0;i<3;i++){memset(&g_list.songs[i],0,sizeof(Song));g_list.songs[i].id=200+i;}
 play_index(0);browse();play_next(1);assert(requested_id==201);play_prev();assert(requested_id==200);
 g_app.busy=0;g_app.time=2;prefetch_tick();assert(prefetch_count==1&&requested_id==201);
 g_app.play_mode=1;g_app.time=4;prefetch_tick();long long planned=requested_id;play_next(1);assert(requested_id==planned);
 puts("PASS actual main playback: cache/local snapshots survive browsing and reorder, stable cache IDs, manual L/R, sequential end, online prefetch plan");return 0;
}
'''
(O/'test.c').write_text(code, encoding='utf-8')
exe = O/'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-I'+str(R/'src'),str(O/'test.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],cwd=O,check=True)
# EOF must continue sharing the manual next context; loop-one remains an audio replay.
eof=s[s.index('if (audio_state() == AUDIO_END'):s.index('if (audio_state() == AUDIO_ERROR',s.index('if (audio_state() == AUDIO_END'))]
assert 'play_next(1)' in eof and 'audio_replay_current(&g_app.now)' in eof and 'audio_seek(0)' not in eof
print('PASS EOF shared next path and loop-one replay retained')
