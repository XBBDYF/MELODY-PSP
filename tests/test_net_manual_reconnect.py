"""Production explicit reconnect must not accept an old GOT_IP association."""
from pathlib import Path
import subprocess,tempfile
from net_fixture import owned_function, CONTROL_STUB
R=Path(__file__).resolve().parents[1];s=(R/'src/net.c').read_text(encoding='utf-8');a=s.index('int net_manual_connect(');b=s.index('\nstatic void dns_expire(',a)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PSP_NET_APCTL_STATE_DISCONNECTED 0
#define PSP_NET_APCTL_STATE_GOT_IP 4
static void mlog(const char*fmt,...){(void)fmt;}
static int g_net_ok=1,g_auto_association_owned=1,g_reconnecting,g_dns_n=2,disconnects,connects,idles,delay_calls,stuck,cancel,wlan=1,state=4,saved_dns;static unsigned g_last_reconn_us=999;
static struct{int ip;}g_dns[4]={{111},{222}};
static int net_wlan_switch(void){return wlan;}
static int net_apctl_state(void){return state;}
static void net_drop_idle(void){idles++;}
static void dns_save(void){assert(!g_dns_n&&!g_dns[0].ip&&!g_dns[1].ip);saved_dns++;}
static int sceNetApctlDisconnect(void){disconnects++;if(!stuck)state=0;return 0;}
static int sceKernelDelayThread(int us){assert(us==50000);delay_calls++;return 0;}
static int aborted(void){return cancel;}
static int net_apctl_connect(int profile,int seconds){assert(profile==4&&seconds==20&&state==0&&g_reconnecting&&g_auto_association_owned==0);connects++;state=4;return 0;}
'''+CONTROL_STUB+owned_function(s,'net_manual_connect')+r'''
int main(void){assert(net_manual_connect(4,20)==0);assert(disconnects==1&&connects==1&&idles==1&&saved_dns==1&&!g_reconnecting&&!g_last_reconn_us);stuck=1;state=2;assert(net_manual_connect(4,20)==-3);assert(delay_calls==40&&connects==1&&!g_reconnecting);stuck=0;state=0;cancel=1;assert(net_manual_connect(4,20)==-2&&connects==1&&!g_reconnecting);cancel=0;wlan=0;assert(net_manual_connect(4,20)<0&&connects==1);wlan=1;g_reconnecting=1;assert(net_manual_connect(4,20)==-4);puts("PASS production manual reconnect: GOT_IP forced re-association; idle sockets and DNS retired; bounded disconnect; cancel/WLAN/ownership retained");}
'''
with tempfile.TemporaryDirectory(prefix='net-manual-')as t:
 p=Path(t);(p/'test.c').write_text(code);exe=p/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',p/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
api=(R/'src/api.c').read_text(encoding='utf-8');body=api[api.index('static void job_connect('):api.index('static void job_chart(')];assert 'net_manual_connect(profile, 20)'in body;assert 'net_probe_host('not in body;assert 'http_get('in body and 'json_validate('in body
assert 'if (job == JOB_CONNECT || job == JOB_PLAY'in api
print('PASS manual API request has immediate priority and verifies actual HTTP JSON, not TCP-only success')
