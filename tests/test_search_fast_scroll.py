"""Compile production song-search fast movement statements, keep editing separate."""
from pathlib import Path
import re,subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/search-fast';O.mkdir(exist_ok=True);s=(R/'src/main.c').read_text(encoding='utf-8');a=s.index('        /* 结果列表 */',s.index('static void input_search('));b=s.index('        if (pressed & PSP_CTRL_CIRCLE)',a);movement=s[a:b]
code=r'''
#include <assert.h>
#include <stdio.h>
#define PSP_CTRL_UP 1
#define PSP_CTRL_DOWN 2
#define PSP_CTRL_LEFT 4
#define PSP_CTRL_RIGHT 8
static struct{int list_sel;}g_app;static struct{int count;}g_list;
static int btn_repeat(unsigned p,unsigned h,float dt,unsigned key){(void)dt;return (p&key)||(h&key);}
static void input(unsigned pressed,unsigned held,float dt){
'''+movement+r'''
}
int main(void){g_list.count=23;g_app.list_sel=3;input(PSP_CTRL_RIGHT,0,.016f);assert(g_app.list_sel==8);input(0,PSP_CTRL_RIGHT,.016f);assert(g_app.list_sel==13);g_app.list_sel=21;input(PSP_CTRL_RIGHT,0,.016f);assert(g_app.list_sel==22);input(PSP_CTRL_LEFT,0,.016f);assert(g_app.list_sel==17);g_app.list_sel=2;input(PSP_CTRL_LEFT,0,.016f);assert(g_app.list_sel==0);g_list.count=0;input(PSP_CTRL_RIGHT,0,.016f);assert(g_app.list_sel==0);puts("PASS actual song-search Dpad/analogue left/right skips five and clamps empty/boundaries");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
