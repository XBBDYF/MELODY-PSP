"""Production PCM level and background effects, no invented beat or FFT claims."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/music-fx';O.mkdir(exist_ok=True);s=(R/'src/audio.c').read_text(encoding='utf-8');a=s.index('static volatile int g_music_level;');b=s.index('static void audio_skin_output(',a)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <math.h>
#define AUDIO_PLAY 2
static int g_pause,g_state=2,g_audio_faulted,g_mv_held,g_mv_request;
'''+s[a:b]+r'''
#include "skin_music_fx.h"
static int draws,maxh,alphas,clip,unclip;
int gfx_push_clip(int a,int b,int c,int d){assert(a==12&&b==58&&c==468&&d==234);clip++;return 0;}void gfx_pop_clip(void){unclip++;}
void gfx_fill_rect_a(int x,int y,int w,int h,unsigned short c,int a){(void)x;(void)y;(void)w;(void)c;draws++;if(h>maxh)maxh=h;alphas+=a;}
int main(void){short pcm[2048];for(int i=0;i<2048;i++)pcm[i]=0;assert(audio_measure_level(pcm,1024,2)==0);for(int i=0;i<2048;i++)pcm[i]=1000;int quiet=audio_measure_level(pcm,1024,2);for(int i=0;i<2048;i++)pcm[i]=-16000;assert(audio_measure_level(pcm,1024,2)>quiet);for(int i=0;i<2048;i++)pcm[i]=-32768;assert(audio_measure_level(pcm,1024,2)==100);for(int i=0;i<2048;i++)pcm[i]=i%2?16000:0;assert(audio_measure_level(pcm,1024,2)>0);g_music_level=77;assert(audio_level()==77);g_pause=1;assert(!audio_level());g_pause=0;g_mv_held=1;assert(!audio_level());g_mv_held=0;g_audio_faulted=1;assert(!audio_level());g_audio_faulted=0;skin_music_fx_draw(0,1,100,31);assert(!draws);skin_music_fx_draw(2,1,0,31);assert(draws==28&&maxh==2);draws=maxh=alphas=0;skin_music_fx_draw(2,1,100,31);assert(draws==28&&maxh>60&&clip==unclip);draws=0;skin_music_fx_draw(1,1,50,31);assert(draws==36&&clip==unclip);puts("PASS actual PCM amplitude/negative full scale/paused-MV guards; energy-driven wave and bounded rain effects");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-Isrc',O/'test.c',R/'src/skin_music_fx.c','-lm','-o',exe],cwd=R,check=True);subprocess.run([exe],check=True)
