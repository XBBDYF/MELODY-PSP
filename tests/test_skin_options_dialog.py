"""Compile real theme-options popup input/rendering with recorded platform UI."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/skin-options-dialog';O.mkdir(exist_ok=True)
(O/'pspctrl.h').write_text('#define PSP_CTRL_UP 1\n#define PSP_CTRL_DOWN 2\n#define PSP_CTRL_LEFT 4\n#define PSP_CTRL_RIGHT 8\n#define PSP_CTRL_CIRCLE 16\n#define PSP_CTRL_CROSS 32\n')
code=r'''
#include <assert.h>
const char *input_buttons_hint(const char *s){return s;}
#include <stdio.h>
#include <string.h>
#include "skin_options.h"
#include "skin_options_ui.h"
#include "skin_runtime.h"
#include "ui.h"
#include "gfx.h"
#include "pspctrl.h"
static SkinOption items[2];static SkinStyle style;static int adjusts,lastindex,lastdirection,draws,toasts;
int skin_options_count(void){return 2;}const SkinOption*skin_options_at(int n){return &items[n];}int skin_options_adjust(int i,int d){adjusts++;lastindex=i;lastdirection=d;items[i].value+=d;return 0;}
int power_runtime_theme_value(const char*id){(void)id;return -1;}
int power_runtime_theme_option(const char*id,int v){(void)id;(void)v;return 0;}
int skin_options_set(int i,int v){items[i].value=v;return 0;}
void skin_runtime_refresh_palette(void){}void ui_theme_changed(void){}
const SkinStyle*skin_runtime_style(void){return &style;}void ui_video_palette(UiVideoPalette*p){memset(p,0,sizeof(*p));}void ui_toast(const char*s){assert(s[0]);toasts++;}
void gfx_reset_clip(void){}unsigned short gfx_rgb(int a,int b,int c){return a+b+c;}
void gfx_fill_rect_a(int a,int b,int c,int d,unsigned short e,int f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;draws++;}
void gfx_fill_rect(int a,int b,int c,int d,unsigned short e){(void)a;(void)b;(void)c;(void)d;(void)e;draws++;}
void gfx_fill_round(int a,int b,int c,int d,int e,unsigned short f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;draws++;}
int gfx_text(int a,int b,int c,const char*d,unsigned short e){(void)a;(void)b;(void)c;(void)d;(void)e;draws++;return 0;}
int gfx_text_ellipsis(int a,int b,int c,int d,const char*e,unsigned short f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;draws++;return 0;}
int gfx_text_right(int a,int b,int c,const char*d,unsigned short e){(void)a;(void)b;(void)c;(void)d;(void)e;draws++;return 0;}
int main(void){strcpy(style.name,"Test skin");strcpy(items[0].label,"Layout");strcpy(items[1].label,"Tiles");skin_options_dialog_open();assert(skin_options_dialog_active());skin_options_dialog_draw();assert(draws>5);assert(skin_options_dialog_input(PSP_CTRL_RIGHT,0,.016f)&&adjusts==1&&lastindex==0&&lastdirection==1);skin_options_dialog_input(PSP_CTRL_DOWN,0,.016f);skin_options_dialog_input(PSP_CTRL_LEFT,0,.016f);assert(adjusts==2&&lastindex==1&&lastdirection==-1);skin_options_dialog_input(PSP_CTRL_CROSS,0,.016f);assert(!skin_options_dialog_active()&&!skin_options_dialog_input(0,0,0));puts("PASS actual theme popup controls/edit/save delegation and close input isolation");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-variable','-I'+str(O),'-Isrc',O/'test.c',R/'src/skin_options_ui.c','-o',exe],cwd=R,check=True);subprocess.run([exe],check=True)
