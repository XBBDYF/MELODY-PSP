"""Compile actual replay helper: restart complete media with copied path, never seek dead decoder."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];s=(R/'src/audio.c').read_text(encoding='utf-8');assert 'int audio_replay_current(' in s,'repeat needs a fresh-generation replay helper'
a=s.index('int audio_replay_current(');b=s.index('\nvoid audio_pause(',a);body=s[a:b]
pre=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct{int dur_ms;char name[64];}Song;
static char g_path[256]="local.mp3";
static int g_local=1,g_audio_faulted,g_mv_held,g_mv_request,g_gen=7,g_dur_ms=10;
static int done=1,stops,starts,state,pause=1;
static int dl_stream_done(int gen){assert(gen==g_gen);return done;}
static void audio_stop(void){stops++;pause=0;}
static int audio_play_file(const char*p,const Song*s){assert(p!=g_path);assert(!strcmp(p,"local.mp3"));assert(s->dur_ms==55||s->dur_ms==g_dur_ms);g_path[0]=0;snprintf(g_path,sizeof(g_path),"%s",p);starts++;state=1;return ++g_gen;}
'''
main=r'''
int main(void){Song s={55,"title"};assert(audio_replay_current(&s)==8);assert(starts==1&&stops==1&&state==1&&!pause);assert(!strcmp(g_path,"local.mp3"));g_local=0;done=0;assert(audio_replay_current(&s)<0);assert(starts==1&&stops==1);done=1;assert(audio_replay_current(&s)==9);g_audio_faulted=1;assert(audio_replay_current(&s)<0);g_audio_faulted=0;g_mv_held=1;assert(audio_replay_current(&s)<0);g_mv_held=0;g_mv_request=1;assert(audio_replay_current(&s)<0);g_mv_request=0;g_path[0]=0;assert(audio_replay_current(&s)<0);strcpy(g_path,"local.mp3");g_local=1;assert(audio_replay_current(0)==10);puts("PASS replay: copied path, fresh generation, complete-stream guard, codec ownership and zero-duration metadata fallback");}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'t.c').write_text(pre+body+main,encoding='utf-8');subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',str(p/'t.c'),'-o',str(p/'t.exe')],check=True);subprocess.run([str(p/'t.exe')],check=True)
