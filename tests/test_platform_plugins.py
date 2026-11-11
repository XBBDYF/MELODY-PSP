"""Real user-mode probe must fail closed for 6.60 and hidden kernel modules."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as td:
 o=Path(td)
 (o/'pspkernel.h').write_text('typedef int SceUID; unsigned sceKernelDevkitVersion(void);\n')
 (o/'pspmodulemgr.h').write_text('typedef struct {unsigned size;char name[32];} SceKernelModuleInfo;int sceKernelGetModuleIdList(int*,int,int*);int sceKernelQueryModuleInfo(int,SceKernelModuleInfo*);\n')
 (o/'test.c').write_text(r'''
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include "pspmodulemgr.h"
#include "platform_plugins.h"
static int hidden=1,which;
unsigned sceKernelDevkitVersion(void){return 0x06060010;}
int sceKernelGetModuleIdList(int*a,int n,int*c){(void)n;if(hidden)return -1;a[0]=1;*c=1;return 0;}
int sceKernelQueryModuleInfo(int id,SceKernelModuleInfo*m){(void)id;strcpy(m->name,which?"wpa2psp":"ARKCompat");return 0;}
void mlog(const char*f,...){(void)f;}
int main(void){platform_plugins_probe();assert(strstr(platform_plugins_summary(),"6.60"));assert(strstr(platform_plugins_wifi_status(),"6.60"));hidden=0;platform_plugins_probe();assert(strstr(platform_plugins_summary(),"ARK"));which=1;platform_plugins_probe();assert(strstr(platform_plugins_wifi_status(),"wpa2psp"));return 0;}
''',encoding='utf-8')
 subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-I'+str(o),'-I'+str(R/'src'),str(o/'test.c'),str(R/'src/platform_plugins.c'),'-o',str(o/'test.exe')],check=True)
 subprocess.run([str(o/'test.exe')],check=True)
print('PASS plugin probe: real6.60, hidden capability stays unknown, ARK/wpa2 no duplicate')
