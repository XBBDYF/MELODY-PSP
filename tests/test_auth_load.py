"""Execute production login loader using in-memory, synthetic files only."""
import unittest
from test_auth_identity import extract, execute, ROOT
class AuthLoadTests(unittest.TestCase):
    def test_invalid_live_falls_back_and_invalid_backup_never_commits(self):
        api=(ROOT/'src/api.c').read_text(encoding='utf-8')
        execute(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOGIN_FILE "fixture/live"
#define LOGIN_BACKUP "fixture/backup"
#define PSP_O_RDONLY 1
typedef unsigned SceSize;
static char g_login_cookie[1280]="OLD",g_login_name[48]="old";
static long long g_login_uid=11;static int g_login_vip,g_logged_in,imported;
static const char*live="cookie=BAD\nuid=invalid\n";
static const char*backup="cookie=SYNTHETIC_BACKUP\nname=B\nuid=22\nvip=0\n";
static int pos[2];
static int sceIoOpen(const char*p,int f,int m){(void)f;(void)m;int id=!strcmp(p,LOGIN_FILE)?0:1;pos[id]=0;return id;}
static int sceIoRead(int fd,void*b,SceSize n){const char*s=fd?backup:live;int rem=(int)strlen(s)-pos[fd];if(n>7)n=7;if((int)n>rem)n=rem;memcpy(b,s+pos[fd],n);pos[fd]+=n;return n;}
static int sceIoClose(int fd){(void)fd;return 0;}
static int cookie_import(void){imported++;return 0;}
static void net_set_cookie(const char*s){(void)s;}
static void safe_copy(char*d,int n,const char*s){snprintf(d,n,"%s",s);}
'''+extract(api,'login_parse')+extract(api,'login_read_path')+extract(api,'login_load')+r'''
int main(void){
login_load();assert(g_logged_in&&g_login_uid==22&&!strcmp(g_login_cookie,"SYNTHETIC_BACKUP")&&!imported);
char over[1500];memcpy(over,"cookie=",7);memset(over+7,'a',1280);strcpy(over+1287,"\nuid=44\n");backup=over;g_logged_in=0;
login_load();assert(!g_logged_in&&g_login_uid==22&&imported==1);
return 0;}
''')
if __name__=='__main__':unittest.main()
