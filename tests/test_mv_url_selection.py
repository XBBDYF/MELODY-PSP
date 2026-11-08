"""Compile production MV JSON callback and quality selection against actual JSON parser."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/mv-url-test';O.mkdir(exist_ok=True)
s=(R/'src/api.c').read_text(encoding='utf-8');a=s.index('static void mv_cb(');b=s.index('\nstatic void job_mv_detail(',a);callback=s[a:b]
a=s.index('    int requested=mv_resolution(),effective=mv_download_resolution()');b=s.index('    g_mv.state =',a);choose=s[a:b]
prefix=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "api.h"
#include "json.h"
static MvInfo g_mv;static char mv_urls[3][900];static int mv_selected_resolution,manual,automatic;
static void safe_copy(char*d,int n,const char*s){snprintf(d,n,"%s",s);}
static int seg_is(const char*s,int n,const char*w){return n==(int)strlen(w)&&!strncmp(s,w,n);}
static int mv_resolution(void){return manual;}
static int mv_download_resolution(void){return manual?manual:automatic;}
#define mlog(...) ((void)0)
'''
body='\nstatic void choose(void){\n'+choose+'\n(void)requested;\n}\n'
tail=r'''
static void run(const char*j,int pref,int auto_rate,const char*want,int resolution){memset(&g_mv,0,sizeof(g_mv));memset(mv_urls,0,sizeof(mv_urls));manual=pref;automatic=auto_rate;json_walk(j,(int)strlen(j),mv_cb,0);choose();assert(!strcmp(g_mv.url,want)&&mv_selected_resolution==resolution);}
int main(void){const char*j="{\"data\":{\"brs\":{\"480\":\"http://fixture/480\",\"240\":\"http://fixture/240\",\"360\":\"http://fixture/360\"}}}";run(j,0,240,"http://fixture/240",240);run(j,0,360,"http://fixture/360",360);run(j,480,240,"http://fixture/480",480);run("{\"data\":{\"brs\":{\"240\":\"low\",\"480\":\"high\"}}}",360,240,"low",240);puts("PASS actual MV URL parser: automatic measured tier/manual override/order independence/missing-tier fallback");return 0;}
'''
(O/'test.c').write_text(prefix+callback+body+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(R/'src'),O/'test.c',R/'src/json.c','-o',exe],check=True);subprocess.run([exe],check=True)
