"""Execute real manual-connect API job; association alone cannot report online."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];s=(R/'src/api.c').read_text(encoding='utf-8');a=s.index('static void job_connect(int profile)');b=s.index('static void job_chart(',a)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#define API_BASE "http://fixture"
#define BUF_SIZE 4096
#define EV_NET_OK 1
#define EV_NET_FAIL 2
static char buffer[BUF_SIZE],*g_buf=buffer;static int wlan=1,association,mode,event,requests;
static void mlog(const char*s,...){(void)s;}
static void post(int type,int arg,const char*msg){(void)arg;(void)msg;event=type;}
static int net_wlan_switch(void){return wlan;}
static int net_manual_connect(int profile,int seconds){assert(profile==4&&seconds==20);return association;}
static int net_apctl_state(void){return 4;}
static int http_get(const char*url,char*out,int max,int*len,int timeout,char*err,int cap){assert(!strcmp(url,API_BASE "/api/song/detail/?ids=%5B0%5D")&&timeout==6000);(void)max;requests++;if(mode==1){snprintf(err,cap,"timeout");*len=0;return -1;}const char*body=mode==2?"not JSON":"{\"code\":200,\"songs\":[]}";*len=(int)strlen(body);strcpy(out,body);return 0;}
'''+s[a:b]+r'''
int main(void){job_connect(4);assert(event==EV_NET_OK&&requests==1);event=0;mode=1;job_connect(4);assert(event==EV_NET_FAIL&&requests==2);mode=2;job_connect(4);assert(event==EV_NET_FAIL&&requests==3);association=-3;event=0;job_connect(4);assert(event==EV_NET_FAIL&&requests==3);association=0;wlan=0;event=0;job_connect(4);assert(event==EV_NET_FAIL&&requests==3);puts("PASS actual API connect job: valid JSON online, HTTP timeout/malformed response/association failure/WLAN off never false-positive");}
'''
with tempfile.TemporaryDirectory(prefix='manual-ready-')as t:
 p=Path(t);(p/'test.c').write_text(code,encoding='utf-8');exe=p/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(R/'src'),p/'test.c',R/'src/json.c','-o',exe],check=True);subprocess.run([exe],check=True)
