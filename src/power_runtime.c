/* Foreground-only power control. Audio/API workers keep running while blank. */
#include "power_runtime.h"
#include "power_policy.h"
#include "app.h"
#include "log.h"
#include <pspkernel.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <string.h>

/* PSPSDK src/user/pspimpose.h; stubs already in libpspuser.a. */
extern int sceImposeGetBacklightOffTime(void);
extern int sceImposeSetBacklightOffTime(int seconds);
static int original_backlight=-1, backlight_supported;
static void apply_backlight(int seconds) {
    if(original_backlight<0){backlight_supported=0;return;}
    int target=seconds>0?seconds:original_backlight;
    if(target<0)return;
    int r=sceImposeSetBacklightOffTime(target);
    backlight_supported=r>=0;
    mlog("power: backlight timeout=%d result=0x%08X",target,r);
}
static MelodyPowerPolicy policy;
static int initial_cpu, initial_bus, effective_cpu, display_blanked, initialized;
static int clock_failed,last_clock_target;
static unsigned last_clock_attempt;
static void *saved_fb;
static int saved_stride,saved_format;
static const int timeouts[]={0,30,60,120,300,-1};
static const int clocks[]={0,222,266,333};
static int member(int n,const int *v,int count) {
    for(int i=0;i<count;++i)if(n==v[i])return 1;
    return 0;
}
static int valid(int eco,int seconds,int mhz) {
    return (eco==0||eco==1)&&member(seconds,timeouts,6)&&member(mhz,clocks,4);
}
static int save_config(int eco,int seconds,int mhz) {
    FILE *f;int ok;
    io_lock();
    f=fopen(APP_DIR "power.cfg.tmp","wb");
    if(!f){io_unlock();return -1;}
    ok=fprintf(f,"MELODY_POWER_1\n%d %d %d\n",eco,seconds,mhz)>0;
    if(fclose(f)!=0)ok=0;
    if(ok){
        /* PSP/Windows rename does not overwrite: keep old data until tmp is ready. */
        FILE *old=fopen(APP_DIR "power.cfg","rb");
        int exists=old!=NULL;
        if(old)fclose(old);
        if(exists){
            remove(APP_DIR "power.cfg.old");
            if(rename(APP_DIR "power.cfg",APP_DIR "power.cfg.old")!=0)ok=0;
        }
        if(ok&&rename(APP_DIR "power.cfg.tmp",APP_DIR "power.cfg")!=0){
            ok=0;
            if(exists)rename(APP_DIR "power.cfg.old",APP_DIR "power.cfg");
        }
    }
    io_unlock();
    return ok?0:-1;
}
void power_runtime_init(void) {
    FILE *f;char magic[32];int eco,seconds,mhz;
    melody_power_policy_init(&policy,(unsigned)(sceKernelGetSystemTimeWide()/1000));
    initial_cpu=scePowerGetCpuClockFrequencyInt();
    initial_bus=scePowerGetBusClockFrequencyInt();effective_cpu=initial_cpu;
    if(initial_cpu<1)initial_cpu=333;
    if(initial_bus<1)initial_bus=166;
    original_backlight=sceImposeGetBacklightOffTime();
    io_lock();f=fopen(APP_DIR "power.cfg","rb");
    if(!f)f=fopen(APP_DIR "power.cfg.old","rb");
    if(f){if(fgets(magic,sizeof(magic),f)&&!strcmp(magic,"MELODY_POWER_1\n")&&
              fscanf(f,"%d %d %d",&eco,&seconds,&mhz)==3&&valid(eco,seconds,mhz)) {
        policy.eco=eco;policy.timeout_seconds=seconds;policy.clock_mhz=mhz;
    }fclose(f);}io_unlock();
    apply_backlight(policy.timeout_seconds);
    initialized=1;
}
int power_runtime_set(const char *key,int value) {
    int eco=policy.eco,seconds=policy.timeout_seconds,mhz=policy.clock_mhz;
    if(!strcmp(key,"eco"))eco=value;
    else if(!strcmp(key,"timeout"))seconds=value;
    else if(!strcmp(key,"clock"))mhz=value;
    else return -1;
    if(!valid(eco,seconds,mhz)||save_config(eco,seconds,mhz)<0)return -1;
    policy.eco=eco;policy.timeout_seconds=seconds;policy.clock_mhz=mhz;
    policy.last_input_ms=(unsigned)(sceKernelGetSystemTimeWide()/1000);
    policy.blanked=0;
    apply_backlight(seconds);
    mlog("power: eco=%d timeout=%d clock=%d",eco,seconds,mhz);
    return 0;
}
int power_runtime_cycle(int id,int direction) {
    const int *v;int n,old;
    if(id==25)return power_runtime_set("eco",!policy.eco);
    if(id==26){v=timeouts;n=6;old=policy.timeout_seconds;}
    else if(id==27){v=clocks;n=4;old=policy.clock_mhz;}
    else return -1;
    for(int i=0;i<n;++i)if(v[i]==old)
        return power_runtime_set(id==26?"timeout":"clock",v[(i+n+(direction<0?-1:1))%n]);
    return -1;
}
int power_runtime_tick(unsigned now,int input,int movie,int busy,int keep_awake) {
    if(!initialized)return 0;
    int wake=melody_power_policy_tick(&policy,now,input,movie,keep_awake);
    int target=melody_power_clock_target(&policy,movie,busy,333);
    int actual=scePowerGetCpuClockFrequencyInt();
    if(actual>0)effective_cpu=(actual>=target-1&&actual<=target+1)?target:actual; /* Firmware resume may change the hardware clock. */
    if(target!=effective_cpu&&(!clock_failed||target!=last_clock_target||(unsigned)(now-last_clock_attempt)>=1000u)){
        last_clock_attempt=now;last_clock_target=target;
        int bus=target==initial_cpu&&policy.clock_mhz==0&&!policy.eco&&!movie?initial_bus:target/2;
        int r=scePowerSetClockFrequency(target,target,bus);
        clock_failed=r<0;
        if(r>=0)effective_cpu=target;
        mlog("power: clock target=%d actual=%d result=0x%08X",target,scePowerGetCpuClockFrequencyInt(),r);
    }
    /* Tick only the display while keeping it awake; never block power switch. */
    if(movie||keep_awake||policy.timeout_seconds<0||input||(!backlight_supported&&policy.timeout_seconds>0&&!policy.blanked))
        scePowerTick(PSP_POWER_TICK_DISPLAY);
    if(policy.blanked&&!display_blanked){
        if(sceDisplayGetFrameBuf(&saved_fb,&saved_stride,&saved_format,PSP_DISPLAY_SETBUF_IMMEDIATE)>=0&&saved_fb&&
           sceDisplaySetFrameBuf(NULL,saved_stride,saved_format,PSP_DISPLAY_SETBUF_IMMEDIATE)>=0){
            display_blanked=1;mlog("power: framebuffer off; backlight timer supported=%d",backlight_supported);
        }
    }else if(!policy.blanked&&display_blanked){
        sceDisplaySetFrameBuf(saved_fb,saved_stride,saved_format,PSP_DISPLAY_SETBUF_IMMEDIATE);
        display_blanked=0;scePowerTick(PSP_POWER_TICK_DISPLAY);mlog("power: display wake");
    }
    return wake;
}
int power_runtime_render_due(unsigned now,int movie){return melody_power_render_due(&policy,now,movie);}
int power_runtime_eco(void){return policy.eco;}
int power_runtime_timeout(void){return policy.timeout_seconds;}
int power_runtime_clock(void){return policy.clock_mhz;}
int power_runtime_effective_clock(void){return scePowerGetCpuClockFrequencyInt();}
int power_runtime_blanked(void){return policy.blanked;}
void power_runtime_shutdown(void){
    if(!initialized)return;
    initialized=0;
    policy.blanked=0;
    if(display_blanked&&saved_fb)sceDisplaySetFrameBuf(saved_fb,saved_stride,saved_format,PSP_DISPLAY_SETBUF_IMMEDIATE);
    display_blanked=0;
    if(original_backlight>=0)sceImposeSetBacklightOffTime(original_backlight);
    scePowerSetClockFrequency(initial_cpu,initial_cpu,initial_bus);
}

/* Reserved, user-edited theme preference IDs. Applying a theme alone changes nothing. */
int power_runtime_theme_option(const char *id,int value) {
    if(!strcmp(id,"system_eco"))return power_runtime_set("eco",value);
    if(!strcmp(id,"system_timeout")) {
        if(value<0||value>=6)return -1;
        return power_runtime_set("timeout",timeouts[value]);
    }
    if(!strcmp(id,"system_clock")) {
        if(value<0||value>=4)return -1;
        return power_runtime_set("clock",clocks[value]);
    }
    return 0;
}

int power_runtime_backlight_supported(void){return backlight_supported;}
int power_runtime_theme_value(const char *id){
    if(!strcmp(id,"system_eco"))return policy.eco;
    if(!strcmp(id,"system_timeout")){for(int i=0;i<6;++i)if(timeouts[i]==policy.timeout_seconds)return i;}
    if(!strcmp(id,"system_clock")){for(int i=0;i<4;++i)if(clocks[i]==policy.clock_mhz)return i;}
    return -1;
}
