"""Actual worker shutdown must release skin admission only after worker stops."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/skin-worker-stop';O.mkdir(exist_ok=True);s=(R/'src/api.c').read_text(encoding='utf-8');a=s.index('int api_shutdown(void)');b=s.index('\nlong long api_comment_song',a)
code=r'''
#include <assert.h>
#include <stdio.h>
typedef unsigned SceUInt;
static int g_quit,g_abort,g_thid,g_skin_request_busy,g_jpeg_on,forced_mode,stopped,loads;
#define mlog(...) ((void)0)
static int sceKernelWaitThreadEnd(int thread,SceUInt*timeout){(void)thread;(void)timeout;assert(g_abort);if(forced_mode)return -1;stopped=1;return 0;}
static void sceKernelTerminateDeleteThread(int thread){(void)thread;stopped=1;}
static void sceKernelDeleteThread(int thread){(void)thread;assert(stopped);}
static void io_lock_reset(void){}static void api_reset_locks(void){}static void net_reset_locks(void){}static void net_drop_idle(void){}
static void skin_catalog_load_local(void){assert(stopped);loads++;}
'''+s[a:b]+r'''
int main(void){for(int f=0;f<2;f++){forced_mode=f;stopped=loads=g_abort=g_quit=0;g_skin_request_busy=1;g_thid=1;int r=api_shutdown();assert(r==f&&loads==1&&stopped&&g_thid==-1&&!g_skin_request_busy&&g_abort&&g_quit);}puts("PASS actual API shutdown cancels network, waits/stops worker, clears skin busy and recovers local catalog for retry after MV");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
