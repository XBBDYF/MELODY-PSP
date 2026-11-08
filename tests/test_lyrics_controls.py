"""Run shared player/lyrics direction handling and verify nonoverlapping lyrics header."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/lyrics-controls';O.mkdir(exist_ok=True)
s=(R/'src/main.c').read_text(encoding='utf-8');a=s.index('static void playback_direction_input(');b=s.index('\nstatic void input_player(',a)
code=r'''
#include <assert.h>
#include <stdio.h>
#define PSP_CTRL_UP 1
#define PSP_CTRL_DOWN 2
#define PSP_CTRL_LEFT 4
#define PSP_CTRL_RIGHT 8
static int volume,seeks,position=60000,target,g_auto_fix_n;static float g_seek_guard_t,g_auto_fix_t;static struct{float time;}g_app;
static int btn_repeat(unsigned p,unsigned h,float dt,unsigned key){(void)h;(void)dt;return(p&key)!=0;}
static void adjust_volume(int n){volume+=n;}
static int audio_pos_ms(void){return position;}
static void audio_seek(int n){seeks++;target=n;}
'''+s[a:b]+r'''
int main(void){g_app.time=12;playback_direction_input(PSP_CTRL_LEFT,0,.02f);assert(target==45000&&seeks==1&&volume==0&&g_seek_guard_t==12);playback_direction_input(PSP_CTRL_RIGHT,0,.02f);assert(target==75000&&seeks==2&&volume==0);playback_direction_input(PSP_CTRL_UP,0,.02f);assert(volume==10&&seeks==2);playback_direction_input(PSP_CTRL_DOWN,0,.02f);assert(volume==0&&seeks==2);puts("PASS actual shared lyrics/player directions: LeftRight seek15s, UpDown volume, seek recovery grace reset");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
x=s[s.index('static void input_lyrics('):s.index('/* ---- 搜索 ----',s.index('static void input_lyrics('))];assert 'playback_direction_input(pressed,held,dt)'in x
u=(R/'src/ui.c').read_text(encoding='utf-8');x=u[u.index('static void screen_lyrics('):u.index('\nstatic ',u.index('static void screen_lyrics(')+20)];assert '26,8,236' in x and 'SCR_W - 12, 8, g_app.now.artist'not in x and '26,28' in x
print('PASS lyrics header ends at262; speed/clock/battery lanes reserved; artist on separate row')
