"""Real button policy: system default, explicit override, persistence and prompt mapping."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];O=R/'.tmp/input-buttons';O.mkdir(exist_ok=True)
(O/'psputility.h').write_text('#define PSP_SYSTEMPARAM_ID_INT_UNKNOWN 9\nint sceUtilityGetSystemParamInt(int,int*);\n')
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "input_buttons.h"
static int system_cross=1,system_error;
int sceUtilityGetSystemParamInt(int id,int*out){assert(id==9);if(system_error)return -1;*out=system_cross;return 0;}
void io_lock(void){}void io_unlock(void){}
int main(void){
 input_buttons_init();assert(input_buttons_cross_confirm());assert(input_buttons_mode()==-1);
 assert(input_buttons_translate(0x4000u|0x10u)==(0x2000u|0x10u));
 assert(input_buttons_translate(0x2000u)==0x4000u);
 assert(input_buttons_translate(0x6000u)==0x6000u);
 assert(!strcmp(input_buttons_confirm_label(),"×"));assert(!strcmp(input_buttons_cancel_label(),"○"));
 assert(!strcmp(input_buttons_hint("○ 确认 / × 返回"),"× 确认 / ○ 返回"));
 assert(!strcmp(input_buttons_hint("song name"),"song name"));
 assert(input_buttons_set_mode(0)==0);assert(!input_buttons_cross_confirm());
 input_buttons_init();assert(input_buttons_mode()==0&&!input_buttons_cross_confirm());
 assert(input_buttons_set_mode(1)==0);input_buttons_init();assert(input_buttons_cross_confirm());
 assert(input_buttons_set_mode(2)<0);assert(input_buttons_mode()==1);
 assert(input_buttons_set_mode(-1)==0);system_cross=0;input_buttons_init();assert(!input_buttons_cross_confirm());
 remove("buttons.cfg");input_buttons_init();assert(input_buttons_mode()==1);remove("buttons.cfg.old");system_error=1;input_buttons_init();assert(!input_buttons_cross_confirm());
 puts("PASS button mapping: system/override/save, simultaneous keys, hint labels, invalid values, SDK failure fallback");
}
'''
(O/'test.c').write_text(code,encoding='utf-8')
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-I'+str(O),'-I'+str(R/'src'),str(O/'test.c'),str(R/'src/input_buttons.c'),'-o',str(O/'test.exe')],check=True)
with tempfile.TemporaryDirectory() as td:subprocess.run([str(O/'test.exe')],cwd=td,check=True)
