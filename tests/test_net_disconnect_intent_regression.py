"""Manual disconnect revokes automatic reconnect until an explicit connect succeeds."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/net.c").read_text(encoding="utf-8")


def function(name):
    start = SOURCE.index(f"int {name}(")
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


PRE = r'''
#include <assert.h>
#include <stdio.h>
#define PSP_NET_APCTL_STATE_DISCONNECTED 0
#define PSP_NET_APCTL_STATE_SCANNING 1
#define PSP_NET_APCTL_STATE_JOINING 2
#define PSP_NET_APCTL_STATE_GETTING_IP 3
#define PSP_NET_APCTL_STATE_GOT_IP 4
static int g_net_ok=1, g_last_profile=1, g_reconnecting;
static unsigned g_last_reconn_us, clock_us=100,g_auto_association_us;
static int g_auto_association_owned;
static int state, connect_calls, disconnect_calls;
static unsigned sceKernelGetSystemTimeLow(void){return clock_us;}
static int sceNetApctlGetState(int *out){*out=state;return 0;}
static int net_apctl_state(void){return state;}
static int net_wlan_switch(void){return 1;}
static int sceNetApctlConnect(int profile){assert(profile==1);connect_calls++;state=4;return 0;}
static int sceNetApctlDisconnect(void){disconnect_calls++;state=0;return 0;}
static void sceKernelDelayThread(unsigned delay){clock_us+=delay;}
/* DNS readiness is covered by test_net_boot_connect.py. */
static int dns_wait_ready(int deadline_ms){(void)deadline_ms;return 0;}
static void mlog(const char *format,...){(void)format;}
'''
MAIN = r'''
int main(void){
    /* An unexpected link loss is still eligible for automatic recovery. */
    assert(net_reconnect_poll()<0);
    assert(connect_calls==1);
    assert(net_apctl_disconnect()==0);
    assert(disconnect_calls==1);
    clock_us+=10000001;
    assert(net_reconnect_poll()<0);
    assert(connect_calls==1); /* manual disconnect must not kick association */
    assert(net_reconnect()<0);
    assert(connect_calls==1); /* API recovery must honor the same intent */
    /* A subsequent explicit user connection restores future recovery. */
    assert(net_apctl_connect(1,1)==0);
    assert(connect_calls==2);
    state=0;
    clock_us+=10000001;
    assert(net_reconnect_poll()<0);
    assert(connect_calls==3);
    puts("PASS manual disconnect blocks both automatic reconnect paths; explicit connect restores recovery");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="net-intent-") as directory:
    directory = Path(directory)
    c_file = directory / "intent.c"
    executable = directory / "intent.exe"
    functions = "\n".join(function(name) for name in (
        "net_apctl_connect", "net_reconnect_poll", "net_reconnect", "net_apctl_disconnect"
    ))
    c_file.write_text(PRE + functions + MAIN, encoding="utf-8")
    subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror", str(c_file), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
