"""Compile production MV OSD and UI palette mapping with theme colors."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/mv-theme';O.mkdir(exist_ok=True)
def function(s,key):
 a=s.index(key);start=s.index('{',a);depth=1;i=start+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
mv=(R/'src/mv.c').read_text(encoding='utf-8');ui=(R/'src/ui.c').read_text(encoding='utf-8');body=function(mv,'static void mv_osd_draw(');palette=function(ui,'void ui_video_palette(')
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct{unsigned short panel,accent,text,muted,track;}UiVideoPalette;
#define C_PANEL 1
#define C_ACC 2
#define C_TXT 3
#define C_MUTE 4
#define C_LINE 5
'''+palette+r'''
static UiVideoPalette g_mv_palette;
#define OSD_RED (g_mv_palette.accent)
#define OSD_TRACK (g_mv_palette.track)
#define OSD_WHITE 65535
#define OSD_YELLOW (g_mv_palette.accent)
#define MV_OSD_Y0 241
#define MV_H 270
typedef struct{long long position_ms,duration_ms,preview_ms;int seeking,buffering,paused;}PspVideoStatus;
static int g_use_source;
static int mv_stream_cancelled(void){return 0;}static long long mv_stream_total(void){return 100;}static long long mv_stream_available(void){return 75;}
static int store_show_speed(void){return 0;}static int net_speed_kbps(void){return 0;}static unsigned sceKernelGetSystemTimeLow(void){return 0;}
static void osd_text(unsigned short*p,int x,int y,const char*s,int z,unsigned short c){(void)p;(void)x;(void)y;(void)s;(void)z;(void)c;}
static int osd_fmt_time(char*out,int ms){return snprintf(out,16,"%02d:%02d",ms/60000,(ms/1000)%60);}
'''+body+r'''
int main(void){unsigned short frame[512*272];UiVideoPalette p;ui_video_palette(&p);assert(p.panel==1&&p.accent==2&&p.text==3&&p.muted==4&&p.track==5);PspVideoStatus status={2500,10000,0,0,0,0};g_mv_palette=p;memset(frame,0,sizeof(frame));mv_osd_draw(frame,&status);assert(frame[(MV_OSD_Y0+7)*512+20]==2);assert(frame[(MV_OSD_Y0+7)*512+400]==5);g_mv_palette.accent=0x07e0;mv_osd_draw(frame,&status);assert(frame[(MV_OSD_Y0+7)*512+20]==0x07e0);status.buffering=1;mv_osd_draw(frame,&status);assert(frame[(MV_OSD_Y0+7)*512+16]==0x07e0);puts("PASS actual UI palette and video OSD follow changing accent/track; buffering shares theme accent");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
start=mv.index('static int mv_begin(',mv.index('static int mv_begin(')+1);assert mv.index('mv_capture_palette();',start)<mv.index('skin_runtime_suspend();',start)
