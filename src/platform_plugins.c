#include "platform_plugins.h"
#include "log.h"
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <stdio.h>
#include <string.h>
static char summary[128]="固件检测尚未完成";
static char wifi_status[160]="未加载额外联网插件";
void platform_plugins_probe(void){
 unsigned version=sceKernelDevkitVersion();int major=(version>>24)&255,minor=(version>>16)&255,patch=(version>>8)&255;
 SceUID ids[128];int count=0,ark=0,pro=0,wpa=0;
 int r=sceKernelGetModuleIdList(ids,sizeof(ids),&count);
 if(r>=0){if(count>128)count=128;for(int i=0;i<count;++i){SceKernelModuleInfo m;memset(&m,0,sizeof(m));m.size=sizeof(m);if(sceKernelQueryModuleInfo(ids[i],&m)<0)continue;
   if(strstr(m.name,"ARK")||strstr(m.name,"ark"))ark=1;
   if(strstr(m.name,"PROSystemControl")||strstr(m.name,"PROUpdater"))pro=1;
   if(strstr(m.name,"wpa2psp"))wpa=1;
 }}
 snprintf(summary,sizeof(summary),"固件%d.%d%d / %s",major,minor,patch,ark?"检测到ARK":pro?"检测到PRO":"CFW内核能力未确认");
 if(wpa)snprintf(wifi_status,sizeof(wifi_status),"检测到wpa2psp已加载，不重复安装");
 else if(ark)snprintf(wifi_status,sizeof(wifi_status),"ARK已含WPA2支持，不重复装插件");
 else if(major==6&&minor==6&&patch==0)snprintf(wifi_status,sizeof(wifi_status),"6.60：已查6.61 PRO插件不匹配，保留原网络栈");
 else if(major==6&&minor==6&&patch==1)snprintf(wifi_status,sizeof(wifi_status),"6.61：需确认PRO内核ABI后才可加载适配插件");
 else snprintf(wifi_status,sizeof(wifi_status),"无已验证的插件匹配项；继续使用系统联网");
 mlog("plugin probe: %s; %s module_query=%08X",summary,wifi_status,r);
}
const char *platform_plugins_summary(void){return summary;}
const char *platform_plugins_wifi_status(void){return wifi_status;}
