"""Execute the real END dispatch: repeat reopens same item, never seeks dead decoder."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1]
s=(R/'src/main.c').read_text(encoding='utf-8');a=s.index('        if (audio_state() == AUDIO_END && g_app.has_now) {');b=s.index('        if (audio_state() == AUDIO_ERROR',a);body=s[a:b]
pre=r'''
#include <assert.h>
#include <stdio.h>
#define AUDIO_END 4
static struct{int has_now,paused,play_mode,playing_index;int now;}g_app;
static struct{int index,count,source;}g_playback;
static int g_end_done,state=4,duration=0,pos=0,preserve_play_page,replays,nexts,seeks,requests,fail;
int audio_state(void){return state;}int audio_dur_ms(void){return duration;}int audio_pos_ms(void){return pos;}
void audio_seek(int n){(void)n;seeks++;}void audio_pause(int n){(void)n;}
int audio_replay_current(const int *s){assert(s==&g_app.now);replays++;if(fail)return -1;state=1;return 2;}
void api_request_play(int i,const int *s){(void)i;(void)s;requests++;}
void ui_toast(const char*s){(void)s;}void mlog(const char*f,...){(void)f;}
void play_next(int dir){assert(dir==1);nexts++;}
'''
main=r'''
int main(void){g_app.has_now=1;g_app.play_mode=2;tick();assert(replays==1&&nexts==0&&seeks==0&&!g_app.paused);tick();state=4;tick();assert(replays==2);g_end_done=0;state=4;fail=1;tick();assert(replays==3&&requests==1&&nexts==0);tick();assert(requests==1);g_end_done=0;fail=0;g_app.play_mode=0;duration=1000;pos=1000;tick();assert(nexts==1);puts("PASS repeat dispatch: unknown-duration END restarts same song, no dead-thread seek, failed replay queued once");}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'t.c').write_text(pre+'void tick(void){\n'+body+'}\n'+main,encoding='utf-8')
 subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t.exe')],check=True)
 subprocess.run([str(p/'t.exe')],check=True)
