from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/multiselect-input';O.mkdir(exist_ok=True);s=(R/'src/main.c').read_text(encoding='utf-8');a=s.index('static Song g_download_selected[32]');b=s.index('static void input_list(',a)
prefix=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "app.h"
AppState g_app;static int submitted;
static int cache_has(long long id){(void)id;return 0;}
static int dl_has_song(long long id){(void)id;return 0;}
static int api_download_pending(long long id){(void)id;return 0;}
static void ui_toast(const char*s){(void)s;}
static int api_request_download_batch(const Song*s,int n){for(int i=0;i<n;i++)assert(s[i].id>0);submitted+=n;return n;}
'''
tail=r'''
int main(void){Song a={0};a.id=123;snprintf(a.name,sizeof(a.name),"selected metadata");select_download_song(&a);assert(g_app.download_selected_count==1&&app_download_selected(123));a.name[0]='x';assert(!strcmp(g_download_selected[0].name,"selected metadata"));select_download_song(&a);assert(!app_download_selected(123)&&!g_app.download_selected_count);for(int i=0;i<33;i++){a.id=1000+i;select_download_song(&a);}assert(g_app.download_selected_count==32);g_app.download_select_mode=1;submit_selected_downloads();assert(submitted==32&&!g_app.download_select_mode&&!g_app.download_selected_count);puts("PASS production multi-select stores immutable song snapshots, toggle/remove, bound32, submits background batch and exits mode");return 0;}
'''
(O/'test.c').write_text(prefix+s[a:b]+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(R/'src'),O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
