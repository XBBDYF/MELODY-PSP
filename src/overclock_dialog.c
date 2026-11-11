#include "overclock_dialog.h"
#include "overclock_bridge.h"
#include "power_runtime.h"
#include "gfx.h"
#include "log.h"
#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int opened,row,generation,choice,confirmed_plugin,confirmed_table,confirmed_device;
static int max_index=-1;
static char notice[144];
static const int generations[]={0,1,2,3,5};
void overclock_dialog_open(void){
 opened=1;row=0;generation=0;choice=0;confirmed_plugin=confirmed_table=confirmed_device=0;max_index=-1;
 snprintf(notice,sizeof(notice),"外部插件手动调频；此处不会设置400MHz");
 io_lock();
 FILE*f=fopen("ms0:/opicker.id","rb");
 if(f){char b[64],*end;size_t n=fread(b,1,63,f);b[n]=0;fclose(f);long v=strtol(b,&end,10);while(*end=='\r'||*end=='\n'||*end==' ')++end;if(end!=b&&!*end&&v>=0&&v<=46)max_index=(int)v;}
 io_unlock();
}
int overclock_dialog_active(void){return opened;}
int overclock_dialog_input(unsigned pressed){
 if(!opened)return 0;
 if(pressed&PSP_CTRL_CROSS){opened=0;return 1;}
 if((pressed&PSP_CTRL_UP)&&row>0)--row;
 if((pressed&PSP_CTRL_DOWN)&&row<6)++row;
 if(pressed&(PSP_CTRL_LEFT|PSP_CTRL_RIGHT|PSP_CTRL_CIRCLE)){
  int dir=(pressed&PSP_CTRL_LEFT)?-1:1;
  if(overclock_bridge_owns_clock()){
   if((pressed&PSP_CTRL_CIRCLE)&&row==5){overclock_bridge_request_release();snprintf(notice,sizeof(notice),"先在插件L+R+VOL-解除；解除后再选确认");}
   if((pressed&PSP_CTRL_CIRCLE)&&row==6){overclock_bridge_acknowledge_release(1);snprintf(notice,sizeof(notice),"已按用户确认交回时钟控制权");}
   return 1;
  }
  switch(row){
   case 0:generation=(generation+5+dir)%5;break;
   case 1:choice=(choice+3+dir)%3;break;
   case 2:confirmed_plugin=!confirmed_plugin;break;
   case 3:confirmed_table=!confirmed_table;break;
   case 4:confirmed_device=!confirmed_device;break;
   case 5:if(pressed&PSP_CTRL_CIRCLE){
    OverclockBridgeConsent c={generations[generation],confirmed_table,confirmed_plugin,confirmed_device,max_index,1,power_runtime_eco()||power_runtime_locked()};
    int r=overclock_bridge_enable(&c,choice);
    if(!r)snprintf(notice,sizeof(notice),"已交接；请在插件手动选档，实际PLL未验证");
    else snprintf(notice,sizeof(notice),"未交接（%d）：核对机型、同机opicker.id和确认项",r);
   }break;
   default:break;
  }
 }
 return 1;
}
void overclock_dialog_draw(void){
 if(!opened)return;
 unsigned short bg=gfx_rgb(13,17,22),fg=gfx_rgb(225,230,235),dim=gfx_rgb(150,160,170),accent=gfx_rgb(230,170,80);
 gfx_reset_clip();gfx_fill_rect(0,0,480,272,bg);gfx_text(FS_MD,16,12,"外部超频接管（需自行安装插件）",fg);
 char lines[7][96];int khz=overclock_bridge_profile_nominal_khz(choice);
 snprintf(lines[0],96,"硬件代数：%s",generation==0?"未确认":generation==1?"1g":generation==2?"2g":generation==3?"早期3g":"5g");
 snprintf(lines[1],96,"期望名义档：%d.%03d MHz（不是实际测量）",khz/1000,khz%1000);
 snprintf(lines[2],96,"已安装启用opicker：%s",confirmed_plugin?"是":"否");
 snprintf(lines[3],96,"使用上游默认频率表：%s",confirmed_table?"是":"否");
 snprintf(lines[4],96,"opicker.id来自同机：%s（最大索引%d）",confirmed_device?"是":"否",max_index);
 snprintf(lines[5],96,"%s",overclock_bridge_owns_clock()?"请求解除外部控制（先操作插件）":"确认交接时钟控制（不执行超频）");
 snprintf(lines[6],96,"%s",overclock_bridge_owns_clock()?"我已手动解除插件超频，确认交回":"关闭此窗口：按 ×");
 for(int i=0;i<7;++i){int y=44+i*23;if(i==row)gfx_fill_rect(10,y-2,460,21,gfx_rgb(42,46,52));gfx_text_ellipsis(FS_SM,17,y,447,lines[i],i==row?accent:fg);}
 gfx_text_ellipsis(FS_SM,16,219,448,notice,accent);
 gfx_text(FS_SM,16,242,"↑↓选择  ←→调整  ○确认  ×关闭；省电皮肤禁止接管",dim);
}
