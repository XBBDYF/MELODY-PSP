"""Extract actual QR poll: logout inside either fetch must prevent old login."""
import unittest
from test_auth_identity import extract, execute, ROOT
class QrIdentityTests(unittest.TestCase):
    def test_logout_in_poll_or_candidate_fetch_prevents_commit(self):
        api=(ROOT/'src/api.c').read_text(encoding='utf-8')
        execute(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define API_BASE "http://fixture.invalid"
#define EV_QR_STATE 1
#define EV_LOGIN_OK 2
#define EV_INFO 3
static char g_qr_key[64]="fixture",g_qr_cookie[1280]="QR_SEED",g_login_cookie[1280]="OLD",g_login_name[48]="old";
static unsigned g_account_generation=2,g_qr_account_generation=2,g_qr_generation=1;
static int g_qr_state,g_abort,g_lock,g_logged_in=1,g_login_vip,requests,saves,logins,logout_at;
static long long g_login_uid=11,g_liked_pl=111,g_liked_cache_uid=11;static int g_liked_total=7;
static char g_buf[32]="fixture",transport_cookie[1280]="OLD";
typedef struct{long long uid;int vip,code;char name[48];}LoginAccount;
static unsigned api_account_generation(void){return g_account_generation;}
static int lock_take(int l){(void)l;return 1;}static void lock_give(int l){(void)l;}
static void mlog(const char*f,...){(void)f;}
static void safe_copy(char*d,int n,const char*s){snprintf(d,n,"%s",s);}
static void net_set_cookie(const char*s){safe_copy(transport_cookie,sizeof(transport_cookie),s);}
static const char*net_last_set_cookie(void){return "MUSIC_U=CANDIDATE";}
static void qr_cb(void*ud,const char*p,int t,const char*s,long long n){(void)ud;(void)p;(void)t;(void)s;(void)n;}
static void account_candidate_cb(void*ud,const char*p,int t,const char*s,long long n){(void)ud;(void)p;(void)t;(void)s;(void)n;}
static int json_walk(const char*b,int n,void(*cb)(void*,const char*,int,const char*,long long),void*ud){(void)b;(void)n;if(cb==account_candidate_cb){LoginAccount*a=ud;a->code=200;a->uid=22;strcpy(a->name,"new");}return 0;}
static int fetch(const char*u,char*e,int n){(void)u;(void)e;(void)n;requests++;if(requests==logout_at){g_account_generation++;g_qr_generation++;g_logged_in=0;g_login_uid=0;g_login_cookie[0]=0;}return 0;}
static int login_save(void){saves++;return 0;}
static void post(int k,int i,const char*m){(void)i;(void)m;if(k==EV_LOGIN_OK)logins++;}
'''+extract(api,'job_qr_poll')+r'''
static void reset(int at){g_account_generation=2;g_qr_account_generation=2;g_qr_generation=1;g_logged_in=1;g_login_uid=11;strcpy(g_login_cookie,"OLD");strcpy(transport_cookie,"OLD");requests=saves=logins=0;logout_at=at;}
int main(void){reset(1);job_qr_poll(g_qr_generation);assert(requests==1&&!g_logged_in&&!saves&&!logins&&!transport_cookie[0]);
reset(2);job_qr_poll(g_qr_generation);assert(requests==2&&!g_logged_in&&!saves&&!logins&&!transport_cookie[0]);
reset(0);job_qr_poll(g_qr_generation);assert(g_logged_in&&g_login_uid==22&&saves==1&&logins==1&&!strcmp(transport_cookie,"MUSIC_U=CANDIDATE"));
reset(0);g_account_generation=3;job_qr_poll(g_qr_generation);assert(!requests&&!saves&&!logins);return 0;}
''')
if __name__=='__main__':unittest.main()
