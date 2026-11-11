#include "input_buttons.h"
#include "app.h"
#include "log.h"
#include <psputility.h>
#include <stdio.h>
#include <string.h>
static int mode=-1,system_cross;
int input_buttons_cross_confirm(void){return mode<0?system_cross:mode;}
int input_buttons_mode(void){return mode;}
const char *input_buttons_confirm_label(void){return input_buttons_cross_confirm()?"×":"○";}
const char *input_buttons_cancel_label(void){return input_buttons_cross_confirm()?"○":"×";}
unsigned input_buttons_translate(unsigned mask){
    if(!input_buttons_cross_confirm())return mask;
    return (mask&~0x6000u)|((mask&0x2000u)<<1)|((mask&0x4000u)>>1);
}
void input_buttons_init(void){
    int system=0,stored=-1;char magic[40];
    system_cross=sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN,&system)>=0&&system==1;
    mode=-1;
    io_lock();FILE*f=fopen(APP_DIR "buttons.cfg","rb");
    if(!f)f=fopen(APP_DIR "buttons.cfg.old","rb");
    if(f){if(fgets(magic,sizeof(magic),f)&&!strcmp(magic,"MELODY_BUTTONS_1\n")&&fscanf(f,"%d",&stored)==1&&stored>=-1&&stored<=1)mode=stored;fclose(f);}io_unlock();
}
int input_buttons_set_mode(int value){
    if(value< -1||value>1)return -1;
    int ok=0,old_exists=0;
    io_lock();FILE*f=fopen(APP_DIR "buttons.cfg.tmp","wb");
    if(f){ok=fprintf(f,"MELODY_BUTTONS_1\n%d\n",value)>0;if(fclose(f))ok=0;}
    if(ok){
        f=fopen(APP_DIR "buttons.cfg","rb");if(f){old_exists=1;fclose(f);}
        if(old_exists){remove(APP_DIR "buttons.cfg.old");if(rename(APP_DIR "buttons.cfg",APP_DIR "buttons.cfg.old"))ok=0;}
        if(ok&&rename(APP_DIR "buttons.cfg.tmp",APP_DIR "buttons.cfg")){ok=0;if(old_exists)rename(APP_DIR "buttons.cfg.old",APP_DIR "buttons.cfg");}
    }
    io_unlock();if(!ok)return -1;mode=value;return 0;
}
const char *input_buttons_hint(const char *text){
    static char slots[8][1024];static unsigned slot;
    if(!text||!input_buttons_cross_confirm())return text?text:"";
    char*out=slots[(slot++)&7u];size_t n=0;
    while(*text&&n<1020){
        if(!strncmp(text,"○",3)){memcpy(out+n,"×",2);n+=2;text+=3;}
        else if(!strncmp(text,"×",2)){memcpy(out+n,"○",3);n+=3;text+=2;}
        else out[n++]=*text++;
    }
    out[n]=0;return out;
}
