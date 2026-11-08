"""Production scrollbar math and update dialog input contract."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/update-ui';O.mkdir(exist_ok=True)
s=(R/'src/ui.c').read_text(encoding='utf-8');a=s.index('static void scrollbar_draw(');b=s.index('\n/*',a)
main=(R/'src/main.c').read_text(encoding='utf-8');x=main.index('static void input_upd_dialog(');y=main.index('/* ---- 下载 ----',x)
code=r'''
#include <assert.h>
#include <stdio.h>
#include "update_log_view.h"
#define SCR_W 480
#define C_PANEL2 1
#define C_ACC 2
#define PSP_CTRL_CIRCLE 1
#define PSP_CTRL_CROSS 2
#define PSP_CTRL_UP 4
#define PSP_CTRL_DOWN 8
static int thumb,bar_height,state,requests,cancels;
static struct{int upd_dlg,upd_dismissed,upd_scroll;}g_app;
static void gfx_fill_round(int x,int y,int w,int h,int radius,int color){(void)x;(void)w;(void)radius;if(color==C_ACC){thumb=y;bar_height=h;}}
static int api_update_state(void){return state;}
static const char*api_update_changelog(void){return "one\ntwo\nthree\nfour\nfive\nsix\nseven\n";}
static void api_update_abort(void){cancels++;}
static void api_request_update_download(void){requests++;state=7;}
'''+s[a:b]+main[x:y]+r'''
int main(void){int last=-1;for(int i=0;i<=90;++i){scrollbar_draw(30,180,100,10,i);assert(thumb>=last);last=thumb;if(i==45)assert(thumb>30&&thumb<160);}assert(thumb+bar_height==210);state=3;g_app.upd_dlg=1;input_upd_dialog(PSP_CTRL_CIRCLE);assert(state==7&&requests==1&&cancels==0&&g_app.upd_dlg);input_upd_dialog(PSP_CTRL_CIRCLE);assert(requests==1&&cancels==0&&g_app.upd_dlg);input_upd_dialog(PSP_CTRL_CROSS);assert(cancels==1&&!g_app.upd_dlg);state=4;g_app.upd_dlg=1;input_upd_dialog(PSP_CTRL_CIRCLE);assert(cancels==1&&g_app.upd_dlg);input_upd_dialog(PSP_CTRL_CROSS);assert(cancels==2&&!g_app.upd_dlg);puts("PASS actual scrollbar moves at middle/end; update confirm queues once, stays visible, Cross consistently cancels queued/active");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(R/'src'),O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
