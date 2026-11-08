"""Execute the production error/retry block; persistent network errors must not skip songs."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/music-failure-test';O.mkdir(exist_ok=True)
s=(R/'src/main.c').read_text(encoding='utf-8');a=s.index('        if (audio_state() == AUDIO_ERROR && g_app.has_now');b=s.index('\n        /* 缓存已经完整',a);block=s[a:b]
prefix=r'''
#include <stdio.h>
#include <assert.h>
#define AUDIO_ERROR 5
#define AUDIO_PLAY  2
static int fixture_pos;
static int g_play_stable_ms;
static int g_retry_waiting;
static int g_retry_gave_up,g_retry_n,g_prev_btn,held,requests,pauses,notes;
static float g_retry_t;
static struct{int has_now,busy,paused,playing_index;struct{long long id;}now;}g_app;
static int audio_state(void){return AUDIO_ERROR;}
static int audio_pos_ms(void){return fixture_pos;}
static const char*audio_error(void){return "cannot connect fixture";}
static void audio_pause(int p){pauses+=p;}
static void ui_toast(const char*s){(void)s;}
static void mlog(const char*f,...){(void)f;}
static void api_note_play_fail(void){notes++;}
static void api_request_play(int index,const void*s){(void)index;(void)s;requests++;g_app.busy=1;}
static void tick(float dt){
'''
tail=r'''
}
int main(void){g_app.has_now=1;g_app.now.id=115087;for(int i=0;i<60;i++)tick(1.0f/60);assert(g_retry_n==1&&requests==0&&!g_retry_gave_up);for(int i=0;i<60;i++)tick(1.0f/60);assert(requests==1&&g_app.busy&&g_retry_n==1);for(int i=0;i<120;i++)tick(1.0f/60);assert(requests==1);g_app.busy=0;g_retry_waiting=0;for(int i=0;i<110;i++)tick(1.0f/60);assert(requests==2&&g_app.busy);g_app.busy=0;g_retry_waiting=0;tick(1.0f/60);assert(g_retry_gave_up&&g_app.paused&&pauses==1&&notes==1);for(int i=0;i<600;i++)tick(1.0f/60);assert(requests==2&&notes==1&&g_app.now.id==115087);puts("PASS persistent network failure: real 1.5s retry delay, no repeated pending retry, bounded2 requests, pause same song");return 0;}
'''
(O/'test.c').write_text(prefix+block+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)

# A stream can report EV_PLAY_OK and fail on the first decode; that must not reset
# the counter (field log: "retry 1" repeating forever, song never advanced).
ev_ok = s[s.index("case EV_PLAY_OK:"):]
ev_ok = ev_ok[:ev_ok.index("case EV_PLAY_FAIL:")]
assert "g_retry_n = 0" not in ev_ok, "EV_PLAY_OK resets the retry counter again"
assert "audio_pos_ms() > 2000" in s, "no stable-playback gate before clearing retries"
assert "audio error, retry %d: %s" in s, "retry log lost the failure reason"
print("PASS retry policy: EV_PLAY_OK no longer resets, stable 2s playback does, reason logged")
