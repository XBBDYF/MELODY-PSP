"""Compile actual song download controls and state labels against recorded queue calls."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/download-pause-controls';O.mkdir(exist_ok=True);s=(R/'src/main.c').read_text(encoding='utf-8');a=s.index('        if (pressed & PSP_CTRL_CIRCLE)',s.index('        DlItem items[DL_QUEUE_MAX];',s.index('static void input_download(')));b=s.index('        if (pressed & PSP_CTRL_SQUARE)',a);controls=s[a:b]
u=(R/'src/ui.c').read_text(encoding='utf-8');a=u.index('static const char *dl_state_text(');b=u.index('static void screen_download_mv',a);labels=u[a:b]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DL_PENDING 0
#define DL_ACTIVE 1
#define DL_DONE 2
#define DL_FAILED 3
#define DL_PAUSED 4
#define PSP_CTRL_CIRCLE 1
#define PSP_CTRL_TRIANGLE 2
typedef struct{int state,pct;}DlItem;
static struct{int dl_sel;}g_app;
static int pauses,resumes,removes;
static int dl_queue_pause(int i){assert(i==1);pauses++;return 0;}
static int dl_queue_resume(int i){assert(i==1);resumes++;return 0;}
static void dl_queue_cancel(int i){assert(i==1);removes++;}
static int dl_queue_count(void){return 2;}
static void ui_toast(const char*s){assert(s&&s[0]);}
static void input(unsigned pressed,int state){int n=3;DlItem items[3]={{0,0},{state,37},{0,0}};
'''+controls+r'''
}
'''+labels+r'''
int main(void){g_app.dl_sel=1;input(PSP_CTRL_CIRCLE,DL_ACTIVE);assert(pauses==1&&resumes==0&&removes==0);input(PSP_CTRL_CIRCLE,DL_PAUSED);assert(resumes==1&&removes==0);input(PSP_CTRL_TRIANGLE,DL_PAUSED);assert(removes==1);char buf[80];DlItem item={DL_PAUSED,37};assert(strstr(dl_state_text(&item,buf,sizeof(buf)),"37%"));puts("PASS actual Circle pause/resume separated from Triangle remove; paused progress shown");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
