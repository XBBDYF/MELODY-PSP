"""Actual loader selection body: a retired native *session* can load pure CPU.
Hardware fault retention remains a separate precondition, never bypassed.
"""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/loader-switch-test';O.mkdir(exist_ok=True)
s=(R/'tools/prxloader/loader.c').read_text(encoding='utf-8');a=s.index('static int do_mv_load(void)');b=s.index('    if (g_mv_mod < 0) {',s.index('    /* MV PRX 选择',a));body=s[a:b]
# include the if-body selected on failed me load; the first matching end may be nested
# Extract only through initial module selection, ending before common start path.
b=s.index('\n    if (g_mv_mod < 0) {\n        snprintf(b',a);body=s[a:b]
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mv_settings_contract.h"
#define PSP_O_RDONLY 1
typedef struct{unsigned magic;}MvApi;
static int g_mv_retired,g_mv_mod=-1,g_mv_avc,g_mv_me_used,native_avc_tried=1,g_mv_api_value,g_me_ok;
static MvApi*g_mv_api;
static int native_mode,load_calls,bridge_calls;
static char selected[128];
static void log_host(const char*s){(void)s;}
static int sceIoOpen(const char*p,int f,int m){(void)f;(void)m;if(strstr(p,"mvdecoder.txt"))return 3;return -1;}
static int sceIoRead(int f,void*p,int n){(void)f;(void)n;((char*)p)[0]=native_mode?'1':'0';return 1;}
static int sceIoClose(int f){(void)f;return 0;}
static int file_exists(const char*p){(void)p;return 1;}
static int free_kb(void){return 16000;}
static int load_native_avc_once(void){return 0;}
static int load_me_bridge_once(void){bridge_calls++;return 0;}
static int sceKernelLoadModule(const char*p,int n,void*x){(void)n;(void)x;load_calls++;snprintf(selected,sizeof(selected),"%s",p);return 10;}
'''
# The selected prefix intentionally returns before starting the module; real PSP
# module lifecycle is tested by the application playback test separately.
code=prefix+body+'\n(void)api;(void)pp;(void)status;(void)r;(void)sr;return 0;}\n'+r'''
int main(void){(void)g_mv_api_value;(void)g_me_ok;assert(do_mv_load()==0);assert(strstr(selected,"melody_mv565.prx")&&bridge_calls==0&&load_calls==1);g_mv_mod=-1;native_mode=1;assert(do_mv_load()==0&&strstr(selected,"melody_mv_avc.prx"));g_mv_mod=-1;g_mv_retired=1;int before=load_calls;assert(do_mv_load()==-3&&load_calls==before);puts("PASS actual loader: native-ready->pureCPU allowed; CPU->native selects module; sticky retired fault still blocks");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(R/'src'),O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
