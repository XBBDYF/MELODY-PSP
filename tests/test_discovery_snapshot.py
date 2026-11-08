"""Exercise production discovery ownership and request routing in a host C harness."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
src = (root / 'src/api.c').read_text(encoding='utf-8')
assert 'const CardList *api_discovery_cards(void)' in src, 'discovery must own a stable view independent of playlist details'
start = src.index('/* Discovery snapshot ownership. */')
end = src.index('/* End discovery snapshot ownership. */', start)
out = root / '.tmp/discovery-snapshot-test'
out.mkdir(parents=True, exist_ok=True)
prefix = r'''
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "app.h"
static CardList g_cards, g_home_cards;
static int requested, posted;
#define EV_CARDS_OK 1
#define EV_LIST_FAIL 2
static void post(int ev,int arg,const char *text){(void)arg;(void)text;posted=ev;}
static void api_request_recommend_lists(void){requested=-1;}
static void api_request_cat_lists(const char *cat){assert(cat&&*cat);requested=1;}
'''
tail = r'''
int main(void){
 g_home_cards.count=24;strcpy(g_home_cards.title,"Homepage recommendations");g_home_cards.items[0].id=101;
 api_request_discovery(-1);assert(posted==EV_CARDS_OK);assert(api_discovery_cards()==&g_home_cards);
 memset(&g_cards,0,sizeof(g_cards));g_cards.count=1;g_cards.items[0].id=900;
 assert(api_discovery_cards()->count==24&&api_discovery_cards()->items[0].id==101);
 api_request_discovery(3);assert(requested==1);g_cards.count=24;g_cards.items[0].id=303;strcpy(g_cards.title,"Electronic");discovery_cards_publish();
 memset(&g_cards,0,sizeof(g_cards));g_cards.count=1;g_cards.items[0].id=901;
 assert(api_discovery_cards()->count==24&&api_discovery_cards()->items[0].id==303);assert(!strcmp(api_discovery_cards()->title,"Electronic"));
 api_request_discovery(-1);assert(api_discovery_cards()==&g_home_cards);
 g_home_cards.count=0;requested=0;api_request_discovery(-1);assert(requested==-1);
 api_request_discovery(8);assert(requested==-1);
 free(g_square_snapshot);puts("PASS discovery recommendation identity, filtered card snapshot survives detail overwrite, empty-home fetch and invalid category");
}
'''
(out/'test.c').write_text(prefix+src[start:end]+tail,encoding='utf-8')
exe=out/'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'src'),str(out/'test.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)

main=(root/'src/main.c').read_text(encoding='utf-8')
assert 'if(i>=-1&&i<8)' in main
assert 'api_request_discovery(i)' in main
assert 'const CardList *cl = api_discovery_cards();' in main
print('PASS main discovery category contract')
# Run the production screen transition, including list -> discovery restoration.
ctx_start=main.index('static struct { int valid, selection, page, category, focus; } g_discovery_return;')
ctx_end=main.index('static void xmb_go_back(',ctx_start)
ctx_body=main[ctx_start:main.index('static int g_xmb_skin_parent',ctx_start)]+main[main.index('static void set_screen(int sc)',ctx_start):ctx_end]
ctx_prefix=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "app.h"
AppState g_app;
static void skin_transition_request(int a,int b,float t){(void)a;(void)b;(void)t;}
static struct { int valid,kind,selection,scroll,kb_edit,state; char query[48]; } g_search_return;
#define mlog(...) ((void)0)
'''
ctx_tail=r'''
int main(void){
 g_app.screen=SC_SQUARE;g_app.grid_sel=7;g_app.grid_page=1;g_app.cat_sel=3;g_app.cat_focus=1;
 set_screen(SC_LIST);g_app.grid_sel=0;g_app.grid_page=0;g_app.cat_sel=-1;g_app.cat_focus=0;
 set_screen(SC_SQUARE);
 assert(g_app.screen==SC_SQUARE&&g_app.grid_sel==7&&g_app.grid_page==1&&g_app.cat_sel==3&&g_app.cat_focus==1&&g_app.grid_kind==4);
 puts("PASS production list return restores discovery category, page, selection and focus");
}
'''
(out/'return.c').write_text(ctx_prefix+ctx_body+ctx_tail,encoding='utf-8')
ret_exe=out/'return.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'src'),str(out/'return.c'),'-o',str(ret_exe)],check=True)
subprocess.run([str(ret_exe)],check=True)
