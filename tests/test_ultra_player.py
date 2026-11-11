"""Compile the real offline player against bounded host audio/gfx stubs."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class UltraPlayerTest(unittest.TestCase):
    def test_offline_controls_metadata_lyrics_and_errors(self):
        self.assertTrue((ROOT / 'src/ultra_player.c').exists(), 'offline player is missing')
        compiler = shutil.which('gcc')
        self.assertIsNotNone(compiler, 'gcc is required for host behavior tests')
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            (work / 'pspctrl.h').write_text('''
#define PSP_CTRL_SELECT 1
#define PSP_CTRL_START 8
#define PSP_CTRL_LEFT 128
#define PSP_CTRL_RIGHT 32
#define PSP_CTRL_SQUARE 32768
#define PSP_CTRL_UP 16
#define PSP_CTRL_DOWN 64
#define PSP_CTRL_LTRIGGER 256
#define PSP_CTRL_RTRIGGER 512
#define PSP_CTRL_TRIANGLE 4096
#define PSP_CTRL_CIRCLE 8192
#define PSP_CTRL_CROSS 16384
''')
            (work / 'song.lrc').write_text('[00:00.00]first\n[00:01.00]second\n[00:02.00]third\n', encoding='utf-8')
            (work / 'song.mp3').write_bytes(b'jpeg fixture')
            (work / 'song.jpg').write_bytes(b'jpeg fixture')
            harness = r'''
#include <assert.h>
void io_lock(void){}
void io_unlock(void){}
#include <stdio.h>
#include <string.h>
#include "ultra_player.c"
AppState g_app;
SongList g_list;
int g_volume = 60;
static int count, scans, starts, stops, state, pos, fail, paused, volume=60;
static char drawn[8192];
static int decodes, clock_mhz,cache_reads;
int power_runtime_set_ultra_clock(int mhz){clock_mhz=mhz;return mhz;}
int cache_count(void){return 1;}
int cache_snapshot_page(CacheEntry *out,int max,int first){if(first>=1)return 0;int n=1-first;if(n>max)n=max;for(int i=0;i<n;i++){memset(&out[i],0,sizeof(*out));out[i].id=100+first+i;strcpy(out[i].name,first+i?"unfinished":"cached complete");}return n;}
const CacheEntry *cache_get(int i){cache_reads++;static CacheEntry e;cache_snapshot_page(&e,1,i);return &e;}
int cache_complete(long long id){return id==100;}
void cache_path(char *out,int n,int kind,long long id){(void)kind;(void)id;snprintf(out,n,"song.mp3");}
int cache_lyric_read(long long id,char *buf,int max){(void)id;return snprintf(buf,max,"{\"lrc\":{\"lyric\":\"[00:00]cache lyric\\n\"}}");}
int coverjpeg_decode_rgba_scaled(const unsigned char *jpg,int len,unsigned char *out,size_t bytes,int dim,int *w,int *h){(void)jpg;(void)len;(void)bytes;assert(dim==96);out[0]=0;*w=*h=1;decodes++;return 0;}
void gfx_blit_rgba_scaled(int x,int y,int w,int h,const unsigned char *rgba,int sw,int sh,int alpha){(void)x;(void)y;(void)w;(void)h;(void)rgba;(void)sw;(void)sh;(void)alpha;}
int local_count(void){ scans++; return count; }
const char *local_path_of(int i){ return i == 0 ? "song.mp3" : "other.mp3"; }
const char *local_name(int i){ return i == 0 ? "song.mp3" : "other.mp3"; }
void audio_stop(void){stops++;state=AUDIO_IDLE;}
int audio_play_file(const char *p,const Song *s){(void)p; assert(!s->art[0]); starts++;state=fail?AUDIO_ERROR:AUDIO_BUFFER;return fail?-1:1;}
void audio_pause(int on){paused=on;state=on?AUDIO_PAUSE:AUDIO_PLAY;}
int audio_paused(void){return paused;}
int audio_state(void){return state;}
int audio_pos_ms(void){return pos;}
int audio_dur_ms(void){return 120000;}
int audio_volume(void){return volume;}
int audio_set_volume(int v){volume=v;return v;}
const char *audio_error(void){return "decoder failed";}
void gfx_clear(unsigned short c){(void)c;drawn[0]=0;}
int gfx_text_ellipsis(int fs,int x,int y,int w,const char *s,unsigned short c){(void)fs;(void)x;(void)y;(void)w;(void)c;strncat(drawn,s,sizeof(drawn)-strlen(drawn)-2);strcat(drawn,"\n");return 0;}
int gfx_text(int fs,int x,int y,const char *s,unsigned short c){return gfx_text_ellipsis(fs,x,y,480,s,c);}
void gfx_fill_rect(int x,int y,int w,int h,unsigned short c){(void)x;(void)y;(void)w;(void)h;(void)c;}
int main(void){
 ultra_player_init();count=0;g_app.now.id=42;g_app.has_now=1;strcpy(g_app.now.art,"online");
 ultra_player_enter();assert(stops==1 && !g_app.has_now && !g_app.now.id && !g_app.now.art[0]);
 ultra_player_tick(PSP_CTRL_CIRCLE|PSP_CTRL_LTRIGGER,0,0.1f);assert(!starts);
 count=2;ultra_player_enter();assert(scans==2);assert(!starts);
 ultra_player_tick(PSP_CTRL_CIRCLE,0,0.1f);assert(starts==1 && g_app.has_now && !strcmp(g_app.now.name,"song.mp3"));
 state=AUDIO_PLAY;ultra_player_tick(PSP_CTRL_CIRCLE,0,0.1f);assert(paused && g_app.paused);
 ultra_player_tick(PSP_CTRL_CIRCLE,0,0.1f);assert(!paused);
 ultra_player_tick(PSP_CTRL_UP,0,0.1f);assert(volume>60 && volume<=100);
 ultra_player_tick(PSP_CTRL_TRIANGLE,0,0.1f);pos=1500;ultra_player_tick(0,0,0.1f);ultra_player_draw();
 assert(strstr(drawn,"first") && strstr(drawn,"second") && strstr(drawn,"third"));
 state=AUDIO_END;ultra_player_tick(0,0,0.1f);assert(starts==2 && !strcmp(g_app.now.name,"other.mp3"));
 state=AUDIO_ERROR;for(int i=0;i<100;i++)ultra_player_tick(0,0,0.1f);assert(starts==2 && scans==2);
 fail=1;ultra_player_tick(PSP_CTRL_RTRIGGER,0,0.1f);assert(!g_app.has_now);for(int i=0;i<10;i++)ultra_player_tick(0,0,0.1f);assert(starts==3);
 ultra_player_tick(PSP_CTRL_CROSS,0,0.1f);assert(g_app.screen==SC_CACHE);
 assert(!ultra_player_tick(PSP_CTRL_SELECT,0,0.1f));ultra_player_draw();
 assert(ultra_player_tick(PSP_CTRL_CIRCLE,0,0.1f)==1);
 ultra_player_enter();
 ultra_player_tick(PSP_CTRL_SQUARE,0,0);ultra_player_draw();assert(strstr(drawn,"cached complete") && !strstr(drawn,"unfinished"));int reads_before=cache_reads;for(int f=0;f<120;++f)ultra_player_draw();assert(cache_reads==reads_before);
 fail=0;ultra_player_tick(PSP_CTRL_CIRCLE,0,0);assert(g_app.now.id==100 && !g_app.now.art[0]);
 ultra_player_tick(PSP_CTRL_START,0,0);ultra_player_tick(PSP_CTRL_CIRCLE,0,0);
 ultra_player_tick(PSP_CTRL_DOWN,0,0);ultra_player_tick(PSP_CTRL_CIRCLE,0,0);assert(decodes==1);
 ultra_player_tick(PSP_CTRL_DOWN,0,0);ultra_player_tick(PSP_CTRL_CIRCLE,0,0);assert(clock_mhz==100);
 ultra_player_tick(PSP_CTRL_CIRCLE,0,0);assert(clock_mhz==80);
 ultra_player_tick(PSP_CTRL_CIRCLE,0,0);assert(clock_mhz==66);
 ultra_player_tick(PSP_CTRL_START,0,0);for(int i=0;i<10;i++)ultra_player_draw();assert(decodes==1);
 ultra_player_tick(PSP_CTRL_CROSS,0,0);ultra_player_tick(PSP_CTRL_SQUARE,0,0);ultra_player_tick(PSP_CTRL_CIRCLE,0,0);assert(!g_app.now.id);
 puts("offline player behavior passed");return 0;
}
'''
            (work / 'host.c').write_text(harness, encoding='utf-8')
            executable = work / 'host.exe'
            compiled = subprocess.run([compiler, '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(work), '-I', str(ROOT / 'src'), str(work / 'host.c'),str(ROOT/'src/json.c'), '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(executable)], cwd=work, check=True, capture_output=True, text=True)
            self.assertIn('behavior passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
