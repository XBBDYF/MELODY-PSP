from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/download-batch';O.mkdir(exist_ok=True);s=(R/'src/api.c').read_text(encoding='utf-8');a=s.index('int api_download_pending(');b=s.index('/* ---------------- 在线更新',a)
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
static Song g_download_requests[32];static unsigned char g_download_silent[32];static int g_download_request_n,g_lock,errors;static unsigned g_download_epoch;
static int lock_take(int l){(void)l;return 1;}static void lock_give(int l){(void)l;}
static int cache_has(long long id){return id==999;}
static int dl_has_song(long long id){return id==998;}
static void post(int kind,int index,const char*msg){(void)kind;(void)index;(void)msg;errors++;}
'''
tail=r'''
int main(void){Song songs[40];memset(songs,0,sizeof(songs));for(int i=0;i<40;++i)songs[i].id=i+1;api_request_prefetch(&songs[0]);api_request_prefetch(&songs[1]);api_request_download(&songs[2]);assert(g_download_requests[0].id==3&&!g_download_silent[0]);assert(api_download_pending(1)&&api_pending_download_count()==3);api_request_download(&songs[0]);assert(api_pending_download_count()==3);api_cancel_pending_prefetch();assert(api_pending_download_count()==2&&api_download_pending(1)&&!api_download_pending(2));api_clear_pending_downloads();assert(!api_pending_download_count());assert(api_request_download_batch(songs,40)==32);assert(api_pending_download_count()==32);api_request_download(&songs[39]);assert(errors==1&&api_pending_download_count()==32);api_clear_pending_downloads();songs[0].id=999;songs[1].id=998;assert(api_request_download_batch(songs,3)==1&&api_pending_download_count()==1);puts("PASS production batch32 capacity, unique IDs, manual priority, auto->manual retained, prefetch off preserves manual, clear pending and cached duplicates");return 0;}
'''
(O/'test.c').write_text(prefix+s[a:b]+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(R/'src'),O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
