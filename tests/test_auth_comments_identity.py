"""Extract the real comment worker and execute synthetic stale-owner races."""
from pathlib import Path
import re
import unittest
from test_auth_identity import extract, execute, ROOT
class CommentIdentityTests(unittest.TestCase):
    def test_stale_success_failure_and_committed_owner(self):
        api=(ROOT/'src/api.c').read_text(encoding='utf-8')
        ctx=re.search(r'typedef struct \{\n    int last_hot;.*?\} CmtCtx;',api,re.S).group()
        source=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define API_BASE "http://fixture.invalid"
#define CM_PAGE_SIZE 20
#define MAX_COMMENTS 40
#define EV_COMMENTS_OK 1
#define EV_LIST_FAIL 2
static int g_lock,g_abort,g_comment_more=1,g_comment_off=20,g_comment_total=9,g_comment_norm_n,g_comment_hot_n,g_comment_page=1;
static long long g_comment_song=101,g_comment_requested_song=202;
static unsigned g_comment_request_gen=2;
static char g_comment_name[128]="A",g_comment_requested_name[128]="B",g_buf[20]="fixture";
typedef struct {int unused;} Comment;
static struct {Comment items[40];int count,total;}g_comments;
static struct {struct{long long id;char name[128];}now;}g_app;
static int mode,events,last_kind;static long long last_subject;
static int lock_take(int l){(void)l;return 1;}static void lock_give(int l){(void)l;}
static void post_ex(int k,int i,long long s,unsigned a,unsigned r,const char*m){(void)i;(void)a;(void)r;(void)m;events++;last_kind=k;last_subject=s;}
static void mlog(const char*f,...){(void)f;}
static void safe_copy(char*d,int n,const char*s){snprintf(d,n,"%s",s);}
static int fetch_json(const char*u,char*e,int n){(void)u;(void)e;(void)n;if(mode==1||mode==2)g_comment_request_gen++;return mode==2?-1:0;}
static void comment_cb(void){ }
'''+ctx+r'''
static int json_walk(const char*b,int n,void(*cb)(void),void*ud){(void)b;(void)n;(void)cb;CmtCtx*c=ud;c->code=200;c->norm_count=2;c->total=2;c->more=0;return 0;}
'''+extract(api,'job_comments')+r'''
int main(void){
g_app.now.id=202;strcpy(g_app.now.name,"B");
mode=1;job_comments(202,0,2);assert(events==0 && g_comment_song==101 && !strcmp(g_comment_name,"A"));
g_comment_request_gen=2;mode=2;job_comments(202,0,2);assert(events==0 && g_comment_song==101);
g_comment_request_gen=2;mode=0;job_comments(202,0,2);assert(events==1 && last_subject==202 && last_kind==EV_COMMENTS_OK && g_comment_song==202 && !strcmp(g_comment_name,"B"));
return 0;}
'''
        execute(source)
if __name__=='__main__':unittest.main()
