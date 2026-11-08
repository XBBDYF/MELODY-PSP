"""Exercise real logout without accessing any filesystem or credentials."""
import unittest
from test_auth_identity import execute, ROOT
class AuthLogoutTests(unittest.TestCase):
    def test_logout_invalidates_account_and_removes_recovery_paths(self):
        api=(ROOT/'src/api.c').read_text(encoding='utf-8')
        function=api.split('void api_logout(void)',1)[1].split('static void qr_cb',1)[0]
        execute(r'''
#include <assert.h>
#include <string.h>
#define LOGIN_FILE "fixture/login"
#define LOGIN_PART "fixture/part"
#define LOGIN_BACKUP "fixture/backup"
#define COOKIE_FILE "fixture/import"
static struct{long long id;char name[72],art[208];}g_daily_preview={123,"old daily","old cover"};
static int g_lock,g_logged_in=1,g_login_vip=1,g_liked_total=7;
static unsigned g_account_generation=2,g_qr_generation=1;
static char g_qr_cookie[1280]="QR",g_qr_key[64]="KEY";
static long long g_liked_pl=111,g_liked_cache_uid=11,g_login_uid=11;
static char g_login_cookie[1280]="SYNTHETIC",g_login_name[48]="fixture";
static int removed;
static int lock_take(int x){(void)x;return 1;}static void lock_give(int x){(void)x;}
static void io_lock(void){}static void io_unlock(void){}
static void net_set_cookie(const char*s){assert(!s[0]);}
static int sceIoRemove(const char*p){if(!strcmp(p,LOGIN_FILE))removed|=1;else if(!strcmp(p,LOGIN_PART))removed|=2;else if(!strcmp(p,LOGIN_BACKUP))removed|=4;else if(!strcmp(p,COOKIE_FILE))removed|=8;else assert(0);return 0;}
static void mlog(const char*f,...){(void)f;}
void api_logout(void)
'''+function+r'''
int main(void){api_logout();assert(!g_logged_in&&!g_login_uid&&!g_login_cookie[0]&&!g_login_name[0]&&!g_liked_pl&&!g_liked_total&&!g_liked_cache_uid&&g_account_generation==3&&removed==15&&!g_daily_preview.id&&!g_daily_preview.name[0]&&!g_daily_preview.art[0]);return 0;}
''')
if __name__=='__main__':unittest.main()
