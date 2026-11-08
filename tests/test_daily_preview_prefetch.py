"""Run the production daily-preview parser/job without PSP or network."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/api.c').read_text(encoding='utf-8')
assert 'void api_request_daily_preview(void)' in source, 'missing idle daily preview request API'

def function(name):
    match = re.search(r'^(?:static )?[^\n]*\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

job = function('job_daily_preview')
assert 'g_pending' not in job and 'post(' not in job and 'post_ex(' not in job
worker = function('worker')
assert worker.index('if (g_play.job)') < worker.index('else if (g_jq_n > 0)') < worker.index('g_download_request_n>0') < worker.index('job=JOB_DAILY_PREVIEW')
assert 'g_download_request_n==0' in worker
out = ROOT / '.tmp/daily-preview-test'
out.mkdir(parents=True, exist_ok=True)
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
typedef struct { long long id; char name[72],artist[72],album[44],art[208]; int dur_ms,fee,pop,hot,playable,mv; } Song;
static Song g_daily_preview;
static unsigned g_daily_preview_account,g_daily_preview_request_account,g_daily_preview_retry_at;
static int g_daily_preview_pending,g_daily_preview_inflight,g_daily_preview_attempted;
static unsigned g_account_generation=1,now=100;
static int g_logged_in=1,g_lock,g_abort,fetches,fail,change_account;
static char buffer[4096],*g_buf=buffer;
static unsigned api_account_generation(void){return g_account_generation;}
static unsigned sceKernelGetSystemTimeLow(void){return now;}
static int lock_take(int l){(void)l;return 1;}
static void lock_give(int l){(void)l;}
static int seg_is(const char*p,int n,const char*t){return n==(int)strlen(t)&&!strncmp(p,t,n);}
static void safe_copy(char*d,int n,const char*s){snprintf(d,n,"%s",s?s:"");}
static int fetch_json(const char*u,char*e,int n){(void)u;(void)e;(void)n;fetches++;if(change_account)++g_account_generation;return fail?-1:0;}
#define API_BASE "http://fixture"
'''
code = prefix + '\n'.join(function(n) for n in ['daily_preview_cb','api_daily_preview','api_request_daily_preview','job_daily_preview']) + r'''
static void run(void){unsigned a=g_daily_preview_request_account;g_daily_preview_pending=0;g_daily_preview_inflight=1;job_daily_preview(a);}
int main(void){
 strcpy(g_buf,"{\"data\":{\"dailySongs\":[{\"id\":9876543210,\"name\":\"First\",\"al\":{\"picUrl\":\"http://cover/first\"}},{\"id\":2,\"name\":\"Second\",\"al\":{\"picUrl\":\"http://cover/second\"}}]}}");
 api_request_daily_preview();assert(g_daily_preview_pending);api_request_daily_preview();run();
 const Song*s=api_daily_preview();assert(s&&s->id==9876543210LL&&!strcmp(s->name,"First")&&!strcmp(s->art,"http://cover/first"));
 api_request_daily_preview();assert(!g_daily_preview_pending&&fetches==1);
 ++g_account_generation;assert(!api_daily_preview());api_request_daily_preview();change_account=1;run();assert(!api_daily_preview());change_account=0;
 api_request_daily_preview();fail=1;run();assert(!api_daily_preview());api_request_daily_preview();assert(!g_daily_preview_pending);
 now+=60000000u;api_request_daily_preview();assert(g_daily_preview_pending);fail=0;run();assert(api_daily_preview());
 ++g_account_generation;strcpy(g_buf,"{\"data\":{\"dailySongs\":[]}}");api_request_daily_preview();run();assert(!api_daily_preview());
 ++g_account_generation;strcpy(g_buf,"{\"songs\":[{\"id\":8,\"name\":\"Legacy\",\"album\":{\"picUrl\":\"http://legacy\"}}]}");api_request_daily_preview();run();assert(api_daily_preview()&&api_daily_preview()->id==8);
 ++g_account_generation;api_request_daily_preview();g_abort=1;run();assert(!api_daily_preview());g_abort=0;
 ++g_account_generation;g_logged_in=0;api_request_daily_preview();assert(!g_daily_preview_pending);puts("PASS daily preview first-song fixture, idle scheduling contract, quiet jobs, account invalidation, retry cooldown, empty/legacy/abort responses");
}
'''
(out / 'test.c').write_text(code, encoding='utf-8')
exe = out / 'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'src'), out/'test.c', ROOT/'src/json.c', '-o', exe], check=True)
subprocess.run([exe], check=True)
