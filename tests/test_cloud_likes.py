"""Exercise production cloud-like callbacks and job with controlled service replies."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/cloud-likes-test';O.mkdir(exist_ok=True)
s=(R/'src/api.c').read_text(encoding='utf-8')
def fn(name):
 import re
 m=re.search(r'^(?:static )?[^\n]*\b'+name+r'\([^;]*?\)\s*\{',s,re.M);assert m,name
 a=m.start();i=s.index('{',a)+1;depth=1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
prefix=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "json.h"
#define API_BASE "http://fixture"
#define CLOUD_LIKE_CAPACITY 4096
#define EV_LIKE_OK 1
static long long g_login_uid=123;
static long long cloud_like_ids[CLOUD_LIKE_CAPACITY];
static int cloud_like_n,cloud_like_complete,cloud_like_queued;
static unsigned cloud_like_account,g_account_generation=2;
static int g_logged_in=1,g_lock,g_abort,g_liked_total=1,remote_has=1,reject,fetches,events,last_event;
static char storage[4096],*g_buf=storage;
static unsigned api_account_generation(void){return g_account_generation;}
static int lock_take(int l){(void)l;return 1;}
static void lock_give(int l){(void)l;}
static int seg_is(const char*p,int n,const char*t){return n==(int)strlen(t)&&!strncmp(p,t,n);}
static int fetch_json(const char*u,char*e,int n){(void)u;(void)e;(void)n;fetches++;snprintf(g_buf,sizeof(storage),"{\"code\":200,\"ids\":%s}",remote_has?"[7,115087,9999999999]":"[7,9999999999]");return 0;}
static int fetch(const char*u,char*e,int n){(void)e;(void)n;fetches++;if(!reject)remote_has=strstr(u,"like=true")!=0;snprintf(g_buf,sizeof(storage),"{\"code\":%d}",reject?301:200);return 0;}
static const char*csrf_token(void){return "fixture";}
static void liked_pl_save(void){}
static void post_ex(int ev,int index,long long id,unsigned account,unsigned req,const char*msg){(void)ev;(void)id;(void)account;(void)req;(void)msg;events++;last_event=index;}
#define mlog(...) ((void)0)
'''
code=prefix+'\n'+ '\n\n'.join(fn(n)for n in ['cloud_like_cb','job_likes_refresh','api_song_liked','operation_code_cb','job_like'])+r'''
int main(void){assert(api_song_liked(115087)==-1);job_likes_refresh();assert(cloud_like_complete&&api_song_liked(115087)==1&&api_song_liked(55)==0&&api_song_liked(9999999999LL)==1);job_like("115087|1|2",0);assert(last_event==0&&api_song_liked(115087)==0&&!remote_has);job_like("115087|2|2",1);assert(last_event==1&&api_song_liked(115087)==1&&remote_has);reject=1;job_like("115087|3|2",0);assert(last_event==-1&&api_song_liked(115087)==1&&remote_has);int old=fetches;g_account_generation=3;job_like("115087|4|2",0);assert(fetches==old&&api_song_liked(115087)==-1);g_logged_in=0;assert(api_song_liked(115087)==-1);puts("PASS production cloud membership, like/unlike refresh, reject preserves state, stale account never mutates");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(R/'src'),O/'test.c',R/'src/json.c','-o',exe],check=True);subprocess.run([exe],check=True)
