from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/ui350-regression';O.mkdir(exist_ok=True)
u=(R/'src/ui.c').read_text(encoding='utf-8');a=u.index('static void views_refresh(');b=u.index('\nstatic int cache_view_has',a)
prefix=r'''
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "update_log_view.h"
static struct{int cache_sel;}g_app;static int g_cache_view_first=-1,g_cache_total;
static int g_cache_view_gen=-1,g_cache_view_n,g_dl_view_gen=-1,g_dl_view_n;static int g_cache_view[1],g_dl_view[1],snapshot_calls,pct;static unsigned now;
#define CACHE_MAX 1
#define DL_QUEUE_MAX 1
static int cache_gen(void){return 1;}static int cache_count(void){return 0;}static int cache_snapshot_page(int*p,int n,int first){(void)p;(void)n;(void)first;return 0;}
static int dl_queue_gen(void){return 1;}static int dl_queue_snapshot(int*p,int n){(void)n;p[0]=pct;snapshot_calls++;return 1;}
static unsigned sceKernelGetSystemTimeLow(void){return now;}
'''
tail=r'''
int main(void){now=1;pct=0;views_refresh();assert(g_dl_view[0]==0);pct=37;now=100000;views_refresh();assert(g_dl_view[0]==0);now=200001;views_refresh();assert(g_dl_view[0]==37&&snapshot_calls==2);pct=81;now=400002;views_refresh();assert(g_dl_view[0]==81);char lines[192][112];int total=update_log_lines("one\ntwo\nthree\nfour\nfive\nsix\nseven\n",lines,192);assert(total==7&&!strcmp(lines[6],"seven"));assert(update_log_clamp(100,total)==2&&update_log_clamp(-4,total)==0);char longline[700];memset(longline,'A',699);longline[699]=0;total=update_log_lines(longline,lines,192);assert(total>5);int n=0;for(int i=0;i<total;i++)n+=strlen(lines[i]);assert(n==699);puts("PASS real queue snapshot refreshes progress without generation changes; full update text wraps and clamps visual scrolling");return 0;}
'''
(O/'test.c').write_text(prefix+u[a:b]+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-O2','-Wall','-Wextra','-Werror','-Isrc',O/'test.c','-o',exe],cwd=R,check=True);subprocess.run([exe],check=True)
a=u.index('static void screen_search(');b=u.index('\nstatic ',a+20);t=u[a:b];assert t.index('/* 搜索框 */')<t.index('if(!g_app.kb_edit&&g_app.search_kind)')
m=(R/'src/main.c').read_text(encoding='utf-8');a=m.index('        case 9: {',m.index('static void input_settings('));b=m.index('        case 10:',a);t=m[a:b];assert 'g_app.upd_dlg=1'in t and 'api_request_update_download'not in t
print('PASS all entity searches render common search box before return; settings check opens modal and never auto-downloads')
