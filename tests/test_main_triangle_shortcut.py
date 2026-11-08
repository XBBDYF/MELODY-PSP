"""Compile the production global Triangle branch for all three main menus."""
from pathlib import Path
import subprocess,re
R=Path(__file__).resolve().parents[1];O=R/'.tmp/triangle-shortcut';O.mkdir(exist_ok=True);s=(R/'src/main.c').read_text(encoding='utf-8');m=re.search(r'if\(g_app\.has_now&&\(pressed&PSP_CTRL_TRIANGLE\).*?\n            \}',s,re.S);assert m
code=r'''
#include <assert.h>
#include <stdio.h>
#define PSP_CTRL_TRIANGLE 256u
#define SC_HOME 1
#define SC_MINE 2
#define SC_SEARCH 3
#define SC_PLAYER 4
#define SC_SETTINGS 5
static struct{int screen,has_now;}g_app;
static void set_screen(int page){g_app.screen=page;}
static unsigned input(unsigned pressed){
'''+m[0]+r'''
return pressed;}
int main(void){for(int page=SC_HOME;page<=SC_SEARCH;page++){g_app.has_now=1;g_app.screen=page;if(page==SC_SEARCH)assert(input(PSP_CTRL_TRIANGLE)==PSP_CTRL_TRIANGLE&&g_app.screen==SC_SEARCH);else assert(input(PSP_CTRL_TRIANGLE)==0&&g_app.screen==SC_PLAYER);g_app.has_now=0;g_app.screen=page;assert(input(PSP_CTRL_TRIANGLE)==PSP_CTRL_TRIANGLE&&g_app.screen==page);}g_app.has_now=1;g_app.screen=SC_SETTINGS;assert(input(PSP_CTRL_TRIANGLE)==PSP_CTRL_TRIANGLE&&g_app.screen==SC_SETTINGS);puts("PASS Triangle returns Home/My to player; Search preserves native submit/edit with a current song");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
