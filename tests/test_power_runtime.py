"""Real power adapter with fake hardware: persist settings, restore clocks/backlight and wake."""
from pathlib import Path
import subprocess, tempfile
R=Path(__file__).resolve().parents[1];O=R/'.tmp/power-runtime-test';O.mkdir(parents=True,exist_ok=True)
(O/'pspkernel.h').write_text('unsigned long long sceKernelGetSystemTimeWide(void);\n',encoding='utf-8')
(O/'psppower.h').write_text('''#define PSP_POWER_TICK_DISPLAY 6
int scePowerGetCpuClockFrequencyInt(void);
int scePowerGetBusClockFrequencyInt(void);
int scePowerSetClockFrequency(int,int,int);
int scePowerTick(int);
''',encoding='utf-8')
(O/'pspdisplay.h').write_text('''#define PSP_DISPLAY_SETBUF_IMMEDIATE 0
int sceDisplayGetFrameBuf(void**,int*,int*,int);
int sceDisplaySetFrameBuf(void*,int,int,int);
''',encoding='utf-8')
code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include "power_runtime.h"
static unsigned long long now;
static int cpu=333,bus=166,backlight=180,backlight_result=0,ticks,clock_calls;
static void *fb=(void*)(uintptr_t)0x4000000;
void io_lock(void){} void io_unlock(void){} void mlog(const char*f,...){(void)f;}
unsigned long long sceKernelGetSystemTimeWide(void){return now*1000;}
int scePowerGetCpuClockFrequencyInt(void){return cpu;}
int scePowerGetBusClockFrequencyInt(void){return bus;}
int scePowerSetClockFrequency(int pll,int c,int b){assert(pll==c);++clock_calls;cpu=c;bus=b;return 0;}
int scePowerTick(int type){assert(type==6);++ticks;return 0;}
int sceImposeGetBacklightOffTime(void){return backlight;}
int sceImposeSetBacklightOffTime(int s){if(backlight_result<0)return backlight_result;backlight=s;return 0;}
int sceDisplayGetFrameBuf(void**p,int*s,int*f,int sync){(void)sync;*p=fb;*s=512;*f=0;return 0;}
int sceDisplaySetFrameBuf(void*p,int s,int f,int sync){assert(s==512);(void)f;(void)sync;fb=p;return 0;}
int main(void){
 power_runtime_init();assert(!power_runtime_eco());assert(backlight==180);
 assert(power_runtime_set("clock",1)<0);
 assert(power_runtime_set("timeout",5)<0);
 assert(power_runtime_set("eco",1)==0);
 assert(power_runtime_set("timeout",30)==0);assert(backlight==30);
 power_runtime_tick(10,0,0,0,0);assert(cpu==222);
 /* Successful native timer must not receive a continuous display tick. */
 ticks=0;power_runtime_tick(20,0,0,0,0);assert(ticks==0);
 now=30000;power_runtime_tick((unsigned)now,0,0,0,0);assert(power_runtime_blanked());assert(!fb);
 now=30001;assert(power_runtime_tick((unsigned)now,1,0,0,0));assert(fb);assert(ticks>0);
 power_runtime_tick(30002,0,1,0,0);assert(cpu==333);
 power_runtime_tick(30002,0,0,1,0);assert(cpu==266);
 cpu=265;int calls=clock_calls;power_runtime_tick(30002,0,0,1,0);assert(clock_calls==calls);
 power_runtime_tick(30002,0,1,0,0);assert(cpu==333);
 cpu=111;power_runtime_tick(30003,0,1,0,0);assert(cpu==333);
 ticks=0;power_runtime_tick(30004,0,0,0,1);assert(ticks>0);
 power_runtime_shutdown();assert(backlight==180);assert(cpu==333&&bus==166);
 power_runtime_init();assert(power_runtime_eco());assert(power_runtime_timeout()==30);
 backlight_result=-1;
 assert(power_runtime_set("timeout",60)==0);
 ticks=0;power_runtime_tick(30003,0,0,0,0);assert(ticks>0);
 power_runtime_shutdown();
 backlight_result=0;backlight=-1;power_runtime_init();
 assert(power_runtime_set("timeout",30)==0);assert(backlight==-1);
 power_runtime_shutdown();
 puts("PASS power adapter: config roundtrip, native timer, fallback, wake, video clock, restore");
}
'''
(O/'test.c').write_text(code,encoding='utf-8')
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-I'+str(O),'-I'+str(R/'src'),str(O/'test.c'),str(R/'src/power_runtime.c'),'-o',str(O/'test.exe')],check=True)
with tempfile.TemporaryDirectory() as td:subprocess.run([str(O/'test.exe')],cwd=td,check=True)
