#include "skin_options_ui.h"
#include "power_runtime.h"
#include "skin_options.h"
#include "skin_runtime.h"
#include "gfx.h"
#include "ui.h"
#include <pspctrl.h>
#include <stdio.h>
static int opened,selected;
void skin_options_dialog_open(void){
 for(int i=0;i<skin_options_count();++i){const SkinOption*o=skin_options_at(i);int v=power_runtime_theme_value(o->id);if(v>=0&&v!=o->value)skin_options_set(i,v);}
 opened=1;selected=0;
}
int skin_options_dialog_active(void){return opened;}
int skin_options_dialog_input(unsigned pressed,unsigned held,float dt){
 (void)held;(void)dt;if(!opened)return 0;
 int count=skin_options_count();
 if(pressed&PSP_CTRL_CROSS){opened=0;return 1;}
 if((pressed&PSP_CTRL_UP)&&selected>0)--selected;
 if((pressed&PSP_CTRL_DOWN)&&selected+1<count)++selected;
 if(count>0&&(pressed&(PSP_CTRL_LEFT|PSP_CTRL_RIGHT|PSP_CTRL_CIRCLE))){int old=skin_options_at(selected)->value;int r=skin_options_adjust(selected,(pressed&PSP_CTRL_LEFT)?-1:1);if(r>=0){const SkinOption*o=skin_options_at(selected);if(power_runtime_theme_option(o->id,o->value)<0){skin_options_set(selected,old);r=-1;}}if(r<0)ui_toast("主题设置保存失败，保留原值");else{skin_runtime_refresh_palette();ui_theme_changed();}}
 return 1;
}
void skin_options_dialog_draw(void){
 if(!opened)return;
 UiVideoPalette p;ui_video_palette(&p);gfx_reset_clip();gfx_fill_rect_a(0,0,480,272,gfx_rgb(0,0,0),180);
 gfx_fill_round(34,32,412,208,10,p.panel);gfx_text(FS_LG,50,43,"主题设置",p.text);
 const SkinStyle*s=skin_runtime_style();gfx_text_ellipsis(FS_SM,50,70,375,s->name[0]?s->name:"系统默认",p.muted);
 int n=skin_options_count(),start=selected>2?selected-2:0;
 if(!n){gfx_text(FS_MD,52,113,"当前主题没有可调整选项",p.text);gfx_text(FS_SM,52,140,"支持主题可在这里切换布局、磁贴和效果",p.muted);}
 for(int i=start;i<n&&i<start+4;++i){const SkinOption*o=skin_options_at(i);int y=93+(i-start)*29;char value[80];
  if(i==selected){gfx_fill_round(45,y-2,390,27,4,p.track);gfx_fill_rect(45,y-2,3,27,p.accent);}
  if(o->type==SKIN_OPTION_TOGGLE)snprintf(value,sizeof(value),"%s",o->value?"开启":"关闭");
  else if(o->type==SKIN_OPTION_CHOICE&&o->value>=0&&o->value<o->option_count)snprintf(value,sizeof(value),"%s",o->options[o->value]);
  else snprintf(value,sizeof(value),"%d",o->value);
  gfx_text_ellipsis(FS_MD,55,y,220,o->label,p.text);gfx_text_right(FS_MD,425,y,value,i==selected?p.accent:p.muted);
 }
 gfx_text(FS_SM,51,217,"↑↓选择  ←→/○调整并保存  ×关闭",p.muted);
}
