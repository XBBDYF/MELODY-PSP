"""Production focus follows song identity when paused head yields to pending job."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/download-pause-focus';O.mkdir(exist_ok=True);s=(R/'src/main.c').read_text(encoding='utf-8');a=s.index('static void download_follow_focus(');b=s.index('\n}',a)+2
code=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define DL_QUEUE_MAX 32
typedef struct{long long id;}DlItem;static struct{int dl_sel;}g_app;static long long g_dl_focus_song;static DlItem queue[3];static int n;
static int dl_queue_snapshot(DlItem*out,int max){assert(max==32);memcpy(out,queue,n*sizeof(*out));return n;}
'''+s[a:b]+r'''
int main(void){n=3;queue[0].id=20;queue[1].id=10;queue[2].id=30;g_dl_focus_song=10;g_app.dl_sel=0;download_follow_focus();assert(g_app.dl_sel==1);queue[0].id=30;queue[1].id=20;queue[2].id=10;download_follow_focus();assert(g_app.dl_sel==2);n=2;download_follow_focus();assert(g_app.dl_sel==1&&g_dl_focus_song==20);n=0;download_follow_focus();assert(!g_app.dl_sel&&!g_dl_focus_song);puts("PASS pause/resume focus follows song across queue reorder/removal/empty");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
