"""Execute the real bounded comment parser/worker with deterministic transport."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_auth_identity import extract, ROOT


class ContinuousCommentsTests(unittest.TestCase):
    def test_append_capacity_stale_and_retry(self):
        api = (ROOT / 'src/api.c').read_text(encoding='utf-8')
        ctx = re.search(r'typedef struct \{\n    int last_hot;.*?\} CmtCtx;', api, re.S).group()
        public = api.replace('\nvoid api_request_comments_more(', '\nstatic void api_request_comments_more(').replace('\nint api_comments_has_more(', '\nstatic int api_comments_has_more(').replace('\nint api_comments_loading_more(', '\nstatic int api_comments_loading_more(')
        more_api = '\n'.join(extract(public, name) for name in ('api_request_comments_more', 'api_comments_has_more', 'api_comments_loading_more'))
        source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "json.h"
#define API_BASE "http://fixture.invalid"
#define CM_PAGE_SIZE 20
#define CM_HOT_SLOTS 15
#define EV_COMMENTS_OK_TEST 1
static CommentList g_comments;
static int g_lock,g_abort,g_comment_more,g_comment_off,g_comment_total,g_comment_norm_n,g_comment_hot_n,g_comment_page;
static long long g_comment_song,g_comment_requested_song=55;
static unsigned g_comment_request_gen=1;
static char g_comment_name[128],g_comment_requested_name[128]="song",g_buf[16000];
#define JOB_COMMENTS 99
static int g_busy,queued;static unsigned queued_generation;
static void queue_job(int j,int a,const char*q,const Song*s){(void)j;(void)q;(void)s;queued++;queued_generation=(unsigned)a;}
static const char *response;static int stale,fail,last_arg,events;
static int lock_take(int l){(void)l;return 1;}static void lock_give(int l){(void)l;}
static void post_ex(int k,int a,long long s,unsigned x,unsigned r,const char*m){(void)k;(void)s;(void)x;(void)r;(void)m;last_arg=a;events++;}
static void mlog(const char*f,...){(void)f;}
static void safe_copy(char*d,int n,const char*s){snprintf(d,n,"%s",s);}
static int seg_is(const char*s,int n,const char*k){return (int)strlen(k)==n&&!strncmp(s,k,n);}
static int fetch_json(const char*u,char*e,int n){(void)u;(void)e;(void)n;if(stale)g_comment_request_gen++;snprintf(g_buf,sizeof(g_buf),"%s",response);return fail?-1:0;}
''' + ctx + '\n' + extract(api, 'comment_cb') + '\n' + extract(api, 'job_comments') + '\n' + more_api + r'''
int main(void){
 response="{\"code\":200,\"total\":100,\"more\":true,\"hotComments\":[{\"commentId\":10,\"content\":\"hot\"}],\"comments\":[{\"commentId\":11,\"content\":\"first\"},{\"commentId\":12}]}";
 job_comments(55,0,1);assert(g_comments.count==3&&g_comment_off==2);
 assert(api_comments_has_more()&&!api_comments_loading_more());
 g_busy=1;api_request_comments_more();assert(!queued);g_busy=0;
 api_request_comments_more();assert(queued==1&&queued_generation==1&&api_comments_loading_more());
 api_request_comments_more();assert(queued==1);
 response="{\"code\":200,\"total\":100,\"more\":true,\"comments\":[{\"commentId\":13,\"content\":\"next\"}]}";
 job_comments(55,2,1);assert(g_comments.count==4);assert(g_comments.items[1].id==11&&g_comments.items[3].id==13);assert(last_arg==1);
 stale=1;job_comments(55,3,1);assert(g_comments.count==4&&g_comment_off==3);stale=0;
 fail=1;job_comments(55,3,2);assert(g_comments.count==4&&g_comment_off==3);fail=0;
 response="{\"code\":200,\"total\":100,\"more\":true,\"comments\":[{\"commentId\":13},{\"commentId\":14}]}";
 job_comments(55,3,2);assert(g_comments.count==5&&g_comments.items[4].id==14);
 while(g_comments.count<MAX_COMMENTS){int off=g_comment_off;snprintf(g_buf,sizeof(g_buf),"{\"code\":200,\"total\":1000,\"more\":true,\"comments\":[{\"commentId\":%d}]}",100+off);char b[300];strcpy(b,g_buf);response=b;job_comments(55,off,2);}
 assert(g_comments.count==MAX_COMMENTS&&!g_comment_more);
 response="{\"code\":200,\"total\":0,\"more\":false,\"comments\":[]}";
 job_comments(55,0,2);assert(g_comments.count==0&&!g_comment_more&&last_arg==0);
 puts("continuous append, duplicates, stale fencing, retry, fixed capacity: PASS");return 0;}
'''
        with tempfile.TemporaryDirectory() as directory:
            c = Path(directory) / 'continuous.c'
            exe = Path(directory) / 'continuous.exe'
            c.write_text(source, encoding='utf-8')
            subprocess.run(['gcc', '-std=c99', '-I', str(ROOT / 'src'), str(c), str(ROOT / 'src/json.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
