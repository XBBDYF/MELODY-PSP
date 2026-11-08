"""Production comment getters and input preserve target/committed ownership."""
import unittest
from test_auth_identity import extract, execute, ROOT
class CommentViewTests(unittest.TestCase):
    def test_pending_target_hides_old_content_and_retries_zero(self):
        api=(ROOT/'src/api.c').read_text(encoding='utf-8')
        names=('api_comments','api_comment_total','api_comment_name','api_comment_target','api_comment_song')
        funcs='\n'.join(line for line in api.splitlines() if any(line.startswith(prefix+n+'(') for n in names for prefix in ('const CommentList *','int ','const char *','long long ')))
        main=(ROOT/'src/main.c').read_text(encoding='utf-8')
        execute(r'''
#include <assert.h>
#include <string.h>
#define PSP_CTRL_UP 1
#define PSP_CTRL_DOWN 2
#define PSP_CTRL_SQUARE 4
#define PSP_CTRL_TRIANGLE 8
#define PSP_CTRL_CROSS 16
#define PSP_CTRL_LEFT 32
#define PSP_CTRL_RIGHT 64
#define PSP_CTRL_CIRCLE 128
#define PSP_CTRL_LTRIGGER 256
#define PSP_CTRL_RTRIGGER 512
'''+f'#include "{(ROOT / "src/comment_view.h").as_posix()}"\n#include "{(ROOT / "src/gfx.h").as_posix()}"'+r'''
static CommentList g_comments={.count=5};static const CommentList g_comments_empty;
static CommentView comment_view;
static const CommentReplies empty_replies;
CommentView *api_comment_view(void){return &comment_view;}
const CommentReplies *api_comment_replies(long long cid){(void)cid;return &empty_replies;}
static int reply_requests;
void api_request_comment_replies(long long song,long long cid){assert(song>0&&cid>0);reply_requests++;}
static unsigned api_comment_generation(void){return 1;}
static int api_comment_page(void){return 1;}
int gfx_text_w_n(int fs,const char*s,int n){(void)fs;(void)s;return n*6;}
static long long g_comment_song=101,g_comment_requested_song=202;
static int g_comment_total=55;
static char g_comment_name[128]="A",g_comment_requested_name[128]="B";
AppState g_app;
static int requests,offset_seen;static long long subject_seen;
static int btn_repeat(unsigned p,unsigned h,float d,unsigned b){(void)p;(void)h;(void)d;(void)b;return 0;}
static int api_comments_has_more(void){return g_comment_song==g_comment_requested_song;}
static int api_comments_loading_more(void){return 0;}
static void api_request_comments_more(void){requests++;subject_seen=g_comment_requested_song;offset_seen=40;}
static int api_comment_offset(void){return 40;}
static void api_request_comments(long long s,int off){requests++;subject_seen=s;offset_seen=off;}
static void set_screen(int s){(void)s;}
'''+funcs+extract(main,'comment_input_measure')+extract(main,'input_comments')+r'''
int main(void){assert(api_comments()->count==0&&api_comment_total()==0&&!strcmp(api_comment_name(),"B")&&api_comment_song()==101);
input_comments(PSP_CTRL_CIRCLE,0,0);assert(reply_requests==0);
input_comments(PSP_CTRL_SQUARE,0,0);assert(requests==1&&subject_seen==202&&offset_seen==0);
g_comment_song=202;strcpy(g_comment_name,"B");g_app.cm_more=0;input_comments(PSP_CTRL_SQUARE,0,0);assert(requests==2&&subject_seen==202&&offset_seen==40&&api_comments()->count==5);
return 0;}
''')
if __name__=='__main__':unittest.main()
