"""Execute production launch-root logic for category and internal storage paths."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];s=(R/'tools/prxloader/loader.c').read_text(encoding='utf-8');a=s.index('static int launch_directory(');b=s.index('int main(int argc',a)
code=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
static char pspdir[512],libdir[512];static int calls;
static int sceIoChdir(const char*s){snprintf(pspdir,sizeof(pspdir),"%s",s);calls++;return 0;}
static int chdir(const char*s){snprintf(libdir,sizeof(libdir),"%s",s);return 0;}
'''+s[a:b]+r'''
int main(void){char*a[]={"ms0:/PSP/GAME/CAT_Music/My Melody/EBOOT.PBP"};assert(!launch_directory(1,a));assert(!strcmp(pspdir,"ms0:/PSP/GAME/CAT_Music/My Melody/")&&!strcmp(pspdir,libdir));char*b[]={"ef0:/PSP/GAME/CAT Apps/MELODY/EBOOT.PBP"};assert(!launch_directory(1,b));assert(!strcmp(pspdir,"ef0:/PSP/GAME/CAT Apps/MELODY/"));int n=calls;char*c[]={"EBOOT.PBP"};assert(!launch_directory(1,c)&&calls==n);assert(!launch_directory(0,NULL));puts("PASS actual launch root: nested category, spaces, ms0/ef0, relative argv retains cwd");}
'''
with tempfile.TemporaryDirectory()as t:
 p=Path(t);(p/'test.c').write_text(code);subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',p/'test.c','-o',p/'test.exe'],check=True);subprocess.run([p/'test.exe'],check=True)
for n in ['loader.c','bundle_update.h','native_avc_host.h','mvprx.c']:assert 'ms0:/PSP/GAME/MELODY/'not in(R/'tools/prxloader'/n).read_text(encoding='utf-8')
