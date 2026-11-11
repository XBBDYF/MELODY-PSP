"""Run the real netconf controller against a deterministic host SDK boundary."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
assert (ROOT / 'src/wifi_dialog.c').exists(), 'missing cooperative netconf controller'

STUB = r'''
#ifndef TEST_PSPUTILITY_H
#define TEST_PSPUTILITY_H
#define PSP_UTILITY_DIALOG_NONE 0
#define PSP_UTILITY_DIALOG_INIT 1
#define PSP_UTILITY_DIALOG_VISIBLE 2
#define PSP_UTILITY_DIALOG_QUIT 3
#define PSP_UTILITY_DIALOG_FINISHED 4
#define PSP_SYSTEMPARAM_ID_INT_LANGUAGE 8
#define PSP_SYSTEMPARAM_ID_INT_UNKNOWN 9
typedef struct { unsigned size; int language,buttonSwap,graphicsThread,accessThread,fontThread,soundThread,result; } pspUtilityDialogCommon;
typedef struct { pspUtilityDialogCommon base; int action; void *adhocparam; int hotspot,hotspot_connected,wifisp; } pspUtilityNetconfData;
#define PSP_NETCONF_ACTION_CONNECTAP 0
int sceUtilityGetSystemParamInt(int,int*);
int sceUtilityNetconfInitStart(pspUtilityNetconfData*);
int sceUtilityNetconfGetStatus(void);
int sceUtilityNetconfUpdate(int);
int sceUtilityNetconfShutdownStart(void);
#endif
'''
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <psputility.h>
#include "wifi_dialog.h"
static int offline,ready,status,init_calls,updates,shutdowns,ends,connected,last_end,init_error,shutdown_error;
static pspUtilityNetconfData *params;
int net_offline(void){return offline;}
int net_dialog_begin(void){return ready;}
void net_dialog_end(int ok){ends++;last_end=ok;}
int net_apctl_state(void){return connected?4:0;}
int sceUtilityGetSystemParamInt(int id,int*out){*out=id==8?1:0;return 0;}
int sceUtilityNetconfInitStart(pspUtilityNetconfData*p){params=p;init_calls++;assert(p->base.size==sizeof(*p));assert(p->base.graphicsThread==17);assert(p->base.accessThread==19);assert(p->base.fontThread==18);assert(p->base.soundThread==16);return init_error;}
int sceUtilityNetconfGetStatus(void){return status;}
int sceUtilityNetconfUpdate(int n){assert(n==1);updates++;return 0;}
int sceUtilityNetconfShutdownStart(void){shutdowns++;return shutdown_error;}
int main(void){
 assert(!wifi_dialog_blocks_input());
 offline=1;assert(wifi_dialog_start()<0);assert(!init_calls);offline=0;
 assert(wifi_dialog_start()==0);assert(wifi_dialog_blocks_input());assert(wifi_dialog_start()<0);
 wifi_dialog_tick();wifi_dialog_draw();assert(!init_calls&&!updates);
 ready=1;status=PSP_UTILITY_DIALOG_INIT;wifi_dialog_tick();assert(init_calls==1);wifi_dialog_draw();assert(!updates);
 status=PSP_UTILITY_DIALOG_VISIBLE;wifi_dialog_tick();wifi_dialog_draw();assert(updates==1);
 status=PSP_UTILITY_DIALOG_QUIT;shutdown_error=-5;wifi_dialog_tick();assert(shutdowns==1&&wifi_dialog_blocks_input()&&!ends);
 shutdown_error=0;wifi_dialog_tick();assert(shutdowns==2);wifi_dialog_tick();assert(shutdowns==2);
 status=PSP_UTILITY_DIALOG_FINISHED;wifi_dialog_tick();assert(!ends&&wifi_dialog_blocks_input());
 connected=1;params->base.result=0;status=PSP_UTILITY_DIALOG_NONE;wifi_dialog_tick();assert(ends==1&&last_end==1);assert(wifi_dialog_get_result()==WIFI_DIALOG_RESULT_CONNECTED);assert(!wifi_dialog_blocks_input());
 init_error=-12;assert(wifi_dialog_start()==0);wifi_dialog_tick();assert(ends==2);assert(wifi_dialog_get_state()==WIFI_DIALOG_FAILED);assert(wifi_dialog_error()==-12);assert(!wifi_dialog_blocks_input());
 init_error=0;ready=0;assert(wifi_dialog_start()==0);offline=1;wifi_dialog_tick();assert(ends==3);assert(!wifi_dialog_blocks_input());offline=0;
 ready=1;assert(wifi_dialog_start()==0);status=PSP_UTILITY_DIALOG_INIT;wifi_dialog_tick();status=PSP_UTILITY_DIALOG_VISIBLE;offline=1;wifi_dialog_tick();assert(wifi_dialog_blocks_input());status=PSP_UTILITY_DIALOG_QUIT;wifi_dialog_tick();status=PSP_UTILITY_DIALOG_NONE;wifi_dialog_tick();assert(last_end==0);assert(wifi_dialog_get_result()!=WIFI_DIALOG_RESULT_CONNECTED);
 puts("PASS netconf: waiting ownership, offline denial, thread priorities, visible-only update, shutdown retry, NONE release, result");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='melody-wifi-dialog-') as directory:
    directory = Path(directory)
    for name in ('psputility.h', 'psputility_netconf.h'):
        (directory / name).write_text(STUB if name == 'psputility.h' else '#include <psputility.h>\n', encoding='utf-8')
    (directory / 'pspnet_apctl.h').write_text('#define PSP_NET_APCTL_STATE_GOT_IP 4\n', encoding='utf-8')
    (directory / 'test.c').write_text(HARNESS, encoding='utf-8')
    exe = directory / 'wifi_dialog_test.exe'
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(directory), '-I', str(ROOT / 'src'), str(ROOT / 'src/wifi_dialog.c'), str(directory / 'test.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    assert (ROOT / 'src/wifi_dialog.h').stat().st_size > 0
