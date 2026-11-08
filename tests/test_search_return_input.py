"""Host C regression for production non-song search open/back input routing."""
from pathlib import Path
import ctypes
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = re.search(r"static (?:void|int) " + name + r"\([^;]*?\)\s*\{", source).start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class SearchReturnInput(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "src/main.c").read_text(encoding="utf-8")
        context = re.search(r"typedef struct \{[^}]+\} SearchReturnContext;\s*static SearchReturnContext g_search_return;", source)
        context = context.group(0) if context else ""
        helpers = "\n".join(function(source, name) for name in
                              ("search_return_save", "search_return_accept")
                              if re.search(r"static (?:void|int) " + name + r"\(", source))
        search = function(source, "input_search")
        # Actual entity branch, including production save/open call.
        branch = search[search.index("if(!g_app.kb_edit&&g_app.search_kind)"):]
        branch = branch[:branch.index("\n    if (!g_app.kb_edit)")]
        event = source[source.index("        case EV_DAILY_OK:"):source.index("        case EV_CARDS_OK:")]
        action = re.search(r' if\(!strcmp\(action,"search_open"\)\)[^\n]+', source)
        action = action.group(0) if action else ''
        code = r'''
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define SC_SEARCH 1
#define SC_LIST 2
#define SC_HOME 3
#define SC_SQUARE 13
#define EV_DAILY_OK 4
#define EV_SONGS_OK 5
#define PSP_CTRL_UP 1
#define PSP_CTRL_DOWN 2
#define PSP_CTRL_LEFT 4
#define PSP_CTRL_RIGHT 8
#define PSP_CTRL_CIRCLE 16
#define PSP_CTRL_TRIANGLE 32
#define PSP_CTRL_CROSS 64
struct {int screen,search_kind,list_sel,list_scroll,kb_edit,search_state,list_from;
 int download_select_mode,download_selected_count,spinner,play_all_next,grid_sel,grid_page,cat_sel,cat_focus,grid_kind;float enter,time;
 char query[128];} g_app;
struct {int count;} g_list;
typedef struct {int count;struct {long long id;} items[24];} CardList;
static CardList entities={12,{{101},{102},{103},{104},{105},{106},{107},{108}}};
static void xmb_record(int sc){(void)sc;}
static void skin_transition_request(int a,int b,float t){(void)a;(void)b;(void)t;}
static int opened_kind,opened_id;
static struct{int valid,selection,page,category,focus;}g_discovery_return;
#define mlog(...) ((void)0)
const CardList *api_search_entities(void){return &entities;}
int btn_repeat(unsigned p,unsigned h,float dt,unsigned key){return (p&key)!=0;}
void api_request_search_open(int kind,long long id){opened_kind=kind;opened_id=(int)id;}
void copy_list_from_api(void){g_list.count=20;}
int can_auto_navigate(void){return 1;}
void api_request_check(void){}
void play_index(int i){}
CONTEXT
HELPERS
SET_SCREEN
void input(unsigned pressed,unsigned held,float dt){BRANCH}
void event(void){int kind=EV_SONGS_OK;switch(kind){EVENT}}
void skin_open(const char*action,const char*arg){ACTION}
void reset(int kind){memset(&g_app,0,sizeof(g_app));g_app.screen=SC_SEARCH;
 g_app.search_kind=kind;g_app.list_sel=7;g_app.list_scroll=4;g_app.search_state=2;
 strcpy(g_app.query,"test query");input(PSP_CTRL_CIRCLE,0,0);}
int run(int kind,int leave){reset(kind);
 if(leave==2){g_app.list_sel=0;skin_open("search_open","108");}
 if(leave==1)set_screen(SC_HOME);event();
 if(leave==1)return g_app.screen;
 g_app.list_sel=1;g_app.list_scroll=0;g_app.kb_edit=1;
 set_screen(SC_SEARCH);
 return (g_app.search_kind==kind && g_app.list_sel==7 && g_app.list_scroll==4 &&
 !g_app.kb_edit && g_app.search_state==2 && !strcmp(g_app.query,"test query") &&
 opened_kind==kind && opened_id==108);}
'''
        for token, value in (("CONTEXT", context), ("HELPERS", helpers),
                             ("SET_SCREEN", function(source,"set_screen")),
                             ("BRANCH", branch), ("EVENT", event), ("ACTION", action)):
            code = code.replace(token, value)
        cls.temp = tempfile.TemporaryDirectory(prefix="search-return-input-")
        folder = Path(cls.temp.name)
        harness = folder / "routing.c"
        harness.write_text(code,encoding="utf-8")
        compiler=shutil.which("gcc") or shutil.which("cc")
        if not compiler:
            raise RuntimeError("Existing host C compiler required")
        library=folder/"routing.dll"
        subprocess.run([compiler,"-shared","-O0",str(harness),"-o",str(library)],check=True)
        cls.lib=ctypes.CDLL(str(library))
        cls.lib.run.argtypes=[ctypes.c_int,ctypes.c_int]
        cls.lib.run.restype=ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        import _ctypes
        import os
        handle=cls.lib._handle
        del cls.lib
        if os.name=="nt":_ctypes.FreeLibrary(handle)
        else:_ctypes.dlclose(handle)
        cls.temp.cleanup()

    def test_album_open_back_preserves_entity_selection_and_keyboard(self):
        self.assertEqual(self.lib.run(2,0),1)

    def test_each_non_song_category_preserves_own_selection(self):
        for kind in (1,3,4):
            with self.subTest(kind=kind):self.assertEqual(self.lib.run(kind,0),1)

    def test_skin_search_open_uses_entity_id_and_category(self):
        self.assertEqual(self.lib.run(2,2),1)

    def test_late_open_result_does_not_navigate_after_leaving_search(self):
        self.assertEqual(self.lib.run(2,1),3)


if __name__=="__main__":unittest.main()
