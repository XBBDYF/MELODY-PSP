"""Non-settings skins must not poll every settings value before every draw."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/settings-model-cost';O.mkdir(exist_ok=True);s=(R/'src/ui.c').read_text(encoding='utf-8');a=s.index('    static const char*setting_labels[');b=s.index('    skin_runtime_set_cache(',a)
code=r'''
#include <assert.h>
#include <stdio.h>
#define MELODY_SETTINGS_COUNT 23
#define SC_SETTINGS 9
static struct{int screen;}g_app;static struct{const char*label;int group;}g_set_defs[23];static int polls,sets;
static int skin_runtime_active(void){return 1;}
static void settings_value(int i,char*out,int n){(void)i;(void)n;out[0]=0;polls++;}
static void skin_runtime_set_settings(const char*const*l,const int*g,const char v[][96],int n){(void)l;(void)g;(void)v;assert(n==23);sets++;}
static void frame(void){
'''+s[a:b]+r'''
}
int main(void){for(int page=2;page<=18;++page){g_app.screen=page;for(int i=0;i<60;++i)frame();}assert(polls==23*60&&sets==60);puts("PASS all other skin pages avoid23 settings polls per frame; settings page stays live");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
