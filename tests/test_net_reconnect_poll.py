"""Cooperative reconnect kicks apctl without blocking the download worker."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1]
s=(R/'src/net.c').read_text(encoding='utf-8')
a=s.index('int net_reconnect_poll(') if 'int net_reconnect_poll(' in s else -1
assert a>=0,'download worker needs a nonblocking reconnect entry point'
b=s.index('\nint net_reconnect(',a)
fn=s[a:b]
pre=r'''
#include <assert.h>
#include <stdio.h>
#define PSP_NET_APCTL_STATE_GOT_IP 4
#define PSP_NET_APCTL_STATE_DISCONNECTED 0
static unsigned clock_us=100, g_last_reconn_us,g_auto_association_us;
static int g_auto_association_owned,disconnects;
static int sceNetApctlDisconnect(void){disconnects++;return 0;}
static int g_net_ok=1,g_last_profile=1,g_reconnecting,state,calls,wlan=1;
static unsigned sceKernelGetSystemTimeLow(void){return clock_us;}
static int net_apctl_state(void){return state;}
static int net_wlan_switch(void){return wlan;}
static int sceNetApctlConnect(int profile){assert(profile==1);calls++;return 0;}
static void mlog(const char*f,...){(void)f;}
'''
main=r'''
int main(void){assert(net_reconnect_poll()<0);assert(calls==1);clock_us+=2000000;assert(net_reconnect_poll()<0);assert(calls==1);clock_us+=10000001;assert(net_reconnect_poll()<0);assert(calls==2);state=2;clock_us+=10000001;net_reconnect_poll();assert(calls==2);state=4;assert(net_reconnect_poll()==0);assert(!g_auto_association_owned);state=0;g_reconnecting=1;net_reconnect_poll();assert(calls==2);g_reconnecting=0;wlan=0;net_reconnect_poll();assert(calls==2);state=0;wlan=1;g_reconnecting=0;clock_us+=10000001;net_reconnect_poll();assert(g_auto_association_owned);state=2;clock_us+=14999999;net_reconnect_poll();assert(!disconnects);clock_us+=2;net_reconnect_poll();assert(disconnects==1&&!g_auto_association_owned);clock_us+=30000000;net_reconnect_poll();assert(disconnects==1);state=4;assert(net_reconnect_poll()==0);puts("PASS owned automatic stuck-state deadline; external/manual association retained; nonblocking apctl trigger, 10s backoff, shared API ownership, active association retained");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='reconnect-poll-')as t:
 t=Path(t);c=t/'test.c';e=t/'test.exe';c.write_text(pre+fn+main,encoding='utf-8');subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O0','-Wall','-Wextra','-Werror',c,'-o',e],check=True);subprocess.run([e],check=True)
