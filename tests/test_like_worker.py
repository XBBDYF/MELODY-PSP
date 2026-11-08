"""Execute production like worker with synthetic HTTP and account changes."""
import unittest
from test_auth_identity import extract, execute, ROOT
class LikeWorkerTests(unittest.TestCase):
    def test_queued_account_and_late_results_are_account_scoped(self):
        api=(ROOT/'src/api.c').read_text(encoding='utf-8')
        source=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define API_BASE "http://fixture.invalid"
'''+f'#include "{(ROOT / "src/json.c").as_posix()}"'+r'''
#define EV_LIKE_OK 1
#define CLOUD_LIKE_CAPACITY 4096
static long long cloud_like_ids[CLOUD_LIKE_CAPACITY],g_login_uid=123;
static int cloud_like_n,cloud_like_complete,cloud_like_queued,g_lock,g_liked_total,refreshes,saves;
static unsigned cloud_like_account;
static int lock_take(int lock){(void)lock;return 1;}
static void lock_give(int lock){(void)lock;}
static void liked_pl_save(void){saves++;}
static int seg_is(const char*s,int len,const char*key){return (int)strlen(key)==len&&!strncmp(s,key,len);}
static int g_logged_in=1,g_abort,requests,events,mode,index_seen;
static unsigned current_account=2,account_seen,request_seen;
static long long subject_seen;
static char g_buf[80]="{ \"code\" : 200 }";
static unsigned api_account_generation(void){return current_account;}
static const char*csrf_token(void){return "SYNTHETIC";}
static void mlog(const char*f,...){(void)f;}
static void post_ex(int k,int i,long long id,unsigned a,unsigned r,const char*m){(void)k;(void)m;events++;index_seen=i;subject_seen=id;account_seen=a;request_seen=r;}
static int fetch(const char*u,char*e,int n){(void)u;(void)e;(void)n;requests++;snprintf(g_buf,sizeof(g_buf),"{\"code\":%d}",mode==3?401:200);if(mode==1||mode==2)current_account++;return mode==2?-1:0;}
static int fetch_json(const char*u,char*e,int n){(void)e;(void)n;assert(strstr(u,"/api/song/like/get?uid=123"));refreshes++;strcpy(g_buf,"{\"code\":200,\"ids\":[101]}");return 0;}
'''+extract(api,'cloud_like_cb')+extract(api,'job_likes_refresh')+extract(api,'operation_code_cb')+extract(api,'job_like')+r'''
int main(void){
job_like("101|3|1",1);assert(requests==0 && events==0);
mode=1;job_like("101|3|2",1);assert(requests==1 && events==0);
current_account=2;mode=2;job_like("101|3|2",1);assert(requests==2 && events==0);
current_account=2;mode=0;job_like("101|4|2",1);assert(events==1&&index_seen==1&&subject_seen==101&&account_seen==2&&request_seen==4);
assert(refreshes>0&&cloud_like_account==2&&cloud_like_complete&&cloud_like_n==1&&cloud_like_ids[0]==101&&g_liked_total==1);
int saves_before=saves,refreshes_before=refreshes;
mode=3;job_like("101|5|2",0);assert(events==2&&index_seen==-1&&account_seen==2&&request_seen==5);
assert(saves==saves_before&&refreshes==refreshes_before&&cloud_like_n==1&&g_liked_total==1);
return 0;}
'''
        execute(source)
if __name__=='__main__':unittest.main()
