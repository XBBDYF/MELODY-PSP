"""Execute production comment screen with graphics calls recorded on the host."""
import unittest
from test_auth_identity import ROOT, extract, execute


class CommentHeaderOcclusionTests(unittest.TestCase):
    def test_header_covers_list_last_in_both_themes(self):
        ui = (ROOT / 'src/ui.c').read_text(encoding='utf-8')
        execute(r'''
#include <assert.h>
static const char *input_buttons_hint(const char *s){return s;}
#define MAX_COMMENTS 40
#include <stdio.h>
#include <string.h>
#define SCR_W 480
#define FS_SM 0
#define FS_MD 1
#define FS_LG 2
#define C_PANEL 10
#define C_MUTE 11
#define C_TXT 12
#define C_SUB 13
#define C_ACC 14
#define C_WHITE 15
#define C_PANEL2 16
#define C_LINE 17
typedef struct { long long id; char avatar[8]; } Comment;
typedef struct { int count; Comment items[8]; } CommentList;
typedef struct { unsigned generation; int page,selected; long long expanded; int viewport; } CommentView;
typedef struct { int count,more; Comment items[8]; } CommentReplies;
static CommentList cm; static CommentView view; static CommentReplies replies;
static struct { float cm_scroll; } g_app;
static int metro,seq,last_body,header_fill,header_count,clip_y,clip_end,scope;
static int reset_seen,footer_count,fast_hint,nav_seen;
static char header_text[64];
static const CommentList *api_comments(void){return &cm;}
static CommentView *api_comment_view(void){return &view;}
static unsigned api_comment_generation(void){return 1;}
static int api_comment_page(void){return 1;}
static const CommentReplies *api_comment_replies(long long id){return &replies;}
static int api_comment_total(void){return 120;}
static const char *api_comment_name(void){return "fixture";}
static int is_metro_style(void){return metro;}
static int content_bottom(void){return 240;}
static void panel_bg(void){seq++;}
static int comment_draw_measure(const char*s,int n){return n;}
static int comment_list_height(const CommentList*c,const CommentView*v,const CommentReplies*r,int w,int(*m)(const char*,int)){return 4000;}
static int comment_row_height(const Comment*c,long long id,const CommentReplies*r,int w,int(*m)(const char*,int)){return 1000;}
static int comment_body_height(const Comment*c,int w,int(*m)(const char*,int)){return 120;}
static float comment_scroll_clamp(float s,int h,int v){return s>h-v?h-v:s;}
static int gfx_push_clip(int a,int b,int c,int d){assert(!scope);scope=1;clip_y=b;clip_end=d;return 0;}
static void gfx_pop_clip(void){assert(scope);scope=0;}
static void gfx_set_clip(int a,int b,int c,int d){assert(!scope);clip_y=b;clip_end=d;}
static void gfx_reset_clip(void){assert(!scope);clip_y=0;clip_end=272;reset_seen=1;}
static void gfx_fill_rect(int x,int y,int w,int h,int color){seq++;if(x==0&&y==0&&w==480){assert(reset_seen&&!scope);assert(clip_y==0&&clip_end==h);assert(h==(metro?76:52)&&color==C_PANEL);header_fill=seq;header_count++;}}
static void gfx_text(int f,int x,int y,const char*s,int c){seq++;}
static void gfx_text_right(int f,int x,int y,const char*s,int c){seq++;if(!scope)snprintf(header_text,sizeof(header_text),"%s",s);}
static void gfx_text_ellipsis(int f,int x,int y,int w,const char*s,int c){seq++;}
static void gfx_text_center(int f,int x,int w,int y,const char*s,int c){seq++;}
static void nav_page(const char*t,const char*r,int b){assert(header_fill>last_body);nav_seen++;snprintf(header_text,sizeof(header_text),"%s",r);seq++;}
static void api_request_comment_count(long long id){}
static int api_avatar_slot(const char*s){return 0;}
static const unsigned short *api_avatar_px(int slot){static unsigned short px;return &px;}
static void gfx_blit565_round(int x,int y,int w,int h,const unsigned short*p,int pw,int ph,int rad,int alpha){assert(scope&&clip_y==(metro?76:52));last_body=++seq;}
static void gfx_circle(int x,int y,int r,int c){last_body=++seq;}
static void comment_draw_body(const Comment*c,int x,int y,int width,int selected){assert(scope&&clip_y==(metro?76:52));last_body=++seq;}
static int api_comment_replies_loading(long long id){return 0;}
static int api_comment_replies_failed(long long id){return 0;}
static void gfx_hline(int x,int y,int w,int c){last_body=++seq;}
static void api_request_thumbs(void){}
static void hint_begin(void){assert(!scope&&header_fill>last_body);}
static void hint_item(const char*k,const char*l){assert(strcmp(k,"L/R"));if(!strcmp(k,"←→")){assert(!strcmp(l,"整屏快滚"));fast_hint++;}footer_count++;}
static void hint_end(void){}
''' + extract(ui, 'comment_header_draw') + extract(ui, 'screen_comments') + r'''
int main(void){
for(metro=0;metro<2;metro++)for(int pass=0;pass<4;pass++){
seq=last_body=header_fill=header_count=reset_seen=footer_count=fast_hint=nav_seen=scope=0;
cm.count=pass==3?0:4;for(int i=0;i<4;i++)cm.items[i].id=i+1;
view.generation=1;view.page=1;view.expanded=pass==2?1:0;replies.count=2;
g_app.cm_scroll=pass==0?0:(pass==1?1e9f:100);
screen_comments(0,.016f);
assert(header_count==1&&header_fill>last_body&&!scope);
assert(strstr(header_text,"已加载")&&strstr(header_text,"120"));
assert(!strstr(header_text,"页"));assert(fast_hint==1&&footer_count==4);
assert(nav_seen==!metro);
}return 0;}
''')


if __name__ == '__main__':
    unittest.main()
