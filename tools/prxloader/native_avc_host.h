#ifndef MELODY_NATIVE_AVC_HOST_H
#define MELODY_NATIVE_AVC_HOST_H
#include <psputility.h>
#include <psputility_avmodules.h>
/* Loader-main-thread only. Native firmware is loaded once per App process.
 * Do not combine with the custom me_lab core in the same boot session. */
static int native_avc_tried,native_avc_ready;
static int load_native_avc_once(void){
 if(native_avc_tried)return native_avc_ready?0:-1;
 native_avc_tried=1;
 if(g_me_ok||g_mv_me_used){log_host("native avc refused: custom ME was used; reboot required\n");return -1;}
 int r=sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
 char message[128];snprintf(message,sizeof(message),"native avcodec utility=%08X\n",(unsigned)r);log_host(message);
 /* audio_init already loads AVCODEC for normal music. This exact result
  * means resident, not failed. Keep all other utility errors fatal. */
 if(r<0&&(unsigned)r!=0x80110F02u)return r;
 SceUID gate=sceKernelCreateSema("melody_native_go",0,0,1,NULL);if(gate<0)return gate;
 int fd=sceIoOpen("./cache/native-fw.txt",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);
 if(fd<0){sceKernelDeleteSema(gate);return fd;}r=sceIoClose(fd);if(r<0){sceKernelDeleteSema(gate);return r;}
 int uid=sceKernelLoadModule("./avc_firmware_load.prx",0,NULL);
 snprintf(message,sizeof(message),"native firmware helper load=%08X\n",(unsigned)uid);log_host(message);
 if(uid<0){sceKernelDeleteSema(gate);return uid;}
 int status=-1;r=sceKernelStartModule(uid,sizeof(gate),&gate,&status,NULL);
 if(r<0||status!=0){g_mv_retired=1;return r<0?r:-1;}
 r=sceKernelSignalSema(gate,1);if(r<0){g_mv_retired=1;return r;}
 unsigned long long deadline=sceKernelGetSystemTimeWide()+5000000ull;int completed=0;
 while(sceKernelGetSystemTimeWide()<deadline){
  char buffer[4096];int f=sceIoOpen("./cache/native-fw.txt",PSP_O_RDONLY,0);
  if(f>=0){int n=sceIoRead(f,buffer,sizeof(buffer)-1);sceIoClose(f);if(n>0){buffer[n]=0;if(strstr(buffer,"completed=1\n"))completed=1;}}
  if(completed)break;
  sceKernelDelayThread(20000);
 }
 if(!completed){g_mv_retired=1;log_host("native helper pending; retain code until reboot\n");return -1;}
 status=-1;r=sceKernelStopModule(uid,0,NULL,&status,NULL);
 snprintf(message,sizeof(message),"native firmware helper stop=%08X status=%08X\n",(unsigned)r,(unsigned)status);log_host(message);
 if(r<0||status!=0){g_mv_retired=1;return r<0?r:-1;}
 r=sceKernelUnloadModule(uid);if(r<0){g_mv_retired=1;return r;}
 r=sceKernelDeleteSema(gate);if(r<0){g_mv_retired=1;return r;}
 native_avc_ready=1;return 0;
}
#endif
