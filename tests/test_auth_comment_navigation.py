"""Actual result branch must never navigate after user leaves comments."""
import unittest
from test_auth_identity import execute, ROOT
class CommentNavigationTests(unittest.TestCase):
    def test_matching_result_never_opens_unrelated_screen(self):
        main=(ROOT/'src/main.c').read_text(encoding='utf-8')
        branch=main.split('case EV_COMMENTS_OK:',1)[1].split('case EV_THUMB_OK:',1)[0]
        execute(r'''
#include <assert.h>
#define SC_COMMENTS 9
static struct{int screen,cm_more,cm_scroll;}g_app;
static struct{int index;long long subject_id;unsigned request_generation;}ev;
static long long api_comment_song(void){return 101;}
static unsigned api_comment_generation(void){return 2;}
static int can_auto_navigate(void){return 1;}
static void set_screen(int s){g_app.screen=s;}
static void handle(void){switch(1){case 1:
'''+branch+r'''
}}
int main(void){ev.subject_id=101;ev.request_generation=2;g_app.screen=1;g_app.cm_scroll=8;handle();assert(g_app.screen==1&&g_app.cm_scroll==8);
g_app.screen=SC_COMMENTS;g_app.cm_scroll=8;handle();assert(g_app.cm_scroll==0);ev.index=1;g_app.cm_scroll=88;handle();assert(g_app.cm_scroll==88);return 0;}
''')
if __name__=='__main__':unittest.main()
