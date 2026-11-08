"""Compile production XMB parent lookup and back bridge with actual AppState."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/xmb-navigation';O.mkdir(exist_ok=True)
s=(R/'src/main.c').read_text(encoding='utf-8')
a=s.index('static int g_xmb_skin_parent');b=s.index('static void set_screen(int sc)',a)
c=s.index('static void xmb_go_back(');d=s.index('/* ------------------------------------------------------------------ */',c)
assert 'xmb_record(' not in s and 'g_xmb_history' not in s
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "skin_runtime.h"
AppState g_app;static SkinStyle style;
int skin_runtime_active(void){return style.id[0]!=0;}
const SkinStyle*skin_runtime_style(void){return &style;}
static void set_screen(int sc){g_app.screen=sc;}
'''+s[a:b]+s[c:d]+r'''
int main(void){
 strcpy(style.id,"xmb");
 /* A visited download/settings page is never the player's parent. */
 set_screen(SC_DOWNLOAD);set_screen(SC_SETTINGS);g_app.play_from=SC_LIST;
 set_screen(SC_PLAYER);set_screen(SC_LYRICS);xmb_go_back();assert(g_app.screen==SC_PLAYER);
 xmb_go_back();assert(g_app.screen==SC_LIST);
 g_app.list_from=SC_SQUARE;g_app.grid_sel=5;g_app.cat_sel=4;xmb_go_back();
 assert(g_app.screen==SC_SQUARE&&g_app.grid_sel==5&&g_app.cat_sel==4);
 xmb_go_back();assert(g_app.screen==SC_HOME);xmb_go_back();assert(g_app.screen==SC_HOME);
 int sources[]={SC_LIST,SC_SEARCH,SC_CACHE};
 for(unsigned i=0;i<sizeof(sources)/sizeof(sources[0]);i++){
  g_app.play_from=sources[i];assert(xmb_parent_screen(SC_PLAYER)==sources[i]);
 }
 int list_sources[]={SC_GRID,SC_SQUARE};
 for(unsigned i=0;i<sizeof(list_sources)/sizeof(list_sources[0]);i++){
  g_app.list_from=list_sources[i];assert(xmb_parent_screen(SC_LIST)==list_sources[i]);
 }
 for(int page=-1;page<=SC_COUNT;page++){
  g_app.play_from=page;g_app.list_from=page;
  int parent=xmb_parent_screen(SC_PLAYER);assert(parent!=SC_PLAYER);
  if(page!=SC_LIST&&page!=SC_SEARCH&&page!=SC_CACHE)assert(parent==SC_HOME);
  parent=xmb_parent_screen(SC_LIST);assert(parent!=SC_LIST);
  if(page!=SC_GRID&&page!=SC_SQUARE)assert(parent==SC_HOME);
 }
 assert(xmb_parent_screen(SC_COMMENTS)==SC_PLAYER);
 int roots[]={SC_DOWNLOAD,SC_CACHE,SC_SETTINGS,SC_LOGIN,SC_GRID,SC_SQUARE,SC_SEARCH,SC_MINE,SC_HELP};
 g_app.login_from=SC_MINE;
 for(unsigned i=0;i<sizeof(roots)/sizeof(roots[0]);i++)assert(xmb_parent_screen(roots[i])==SC_HOME);
 g_app.login_from=SC_SETTINGS;assert(xmb_parent_screen(SC_LOGIN)==SC_SETTINGS);
 g_xmb_skin_parent=SC_SETTINGS;assert(xmb_parent_screen(SC_SKINS)==SC_SETTINGS);
 g_xmb_skin_parent=SC_HOME;assert(xmb_parent_screen(SC_SKINS)==SC_HOME);
 g_xmb_skin_parent=SC_SKINS;assert(xmb_parent_screen(SC_SKINS)==SC_HOME);
 g_app.screen=SC_PLAYER;g_app.cards_open=1;g_app.play_all_next=1;xmb_go_back();
 assert(!g_app.cards_open&&!g_app.play_all_next);
 strcpy(style.id,"metro");g_app.screen=SC_SETTINGS;xmb_go_back();assert(g_app.screen==SC_SETTINGS);
 puts("PASS XMB explicit parents, source validation, root clamp, state retention and no visit-history rewind");
 return 0;
}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-Isrc',O/'test.c','-o',exe],cwd=R,check=True)
subprocess.run([exe],check=True)
