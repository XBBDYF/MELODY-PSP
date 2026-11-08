/* Optional update screenshots: HTTP work belongs exclusively to the API worker. */
#include "update_gallery.h"
#include "json.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef UPDATE_GALLERY_HOST_TEST
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#else
#include "app.h"
#include "gfx.h"
#include "png.h"
#include "ui.h"
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#endif
#define BASE "http://melody-1320953126.cos.ap-nanjing.myqcloud.com/melody/update-preview/"
#define PNG_MAX (128*1024)
#define JSON_MAX 8192
#define PIXELS (480*272)
#define UI_BYTES (PNG_MAX+PIXELS*6)
#define CACHE APP_DIR "update-preview/"
#define PENDING APP_DIR "update-preview-pending.txt"
#define SEEN APP_DIR "update-preview-seen.txt"
typedef struct { char file[41],caption[161]; } Slide;
typedef struct { int schema,bad,count; unsigned files; char version[49],revision[49]; Slide slides[8]; } Manifest;
/* State publishes the fully downloaded immutable manifest to the UI. */
static volatile int state, generation;
static char version[49],revision[49];
static Manifest gallery;
static int page, decoded=-1, width,height;
static unsigned char *memory;
static time_t retry_after;
#ifndef UPDATE_GALLERY_HOST_TEST
static SceUID ui_block=-1;
#endif
static void barrier(void){
#ifndef UPDATE_GALLERY_HOST_TEST
    __asm__ volatile("sync" ::: "memory");
#endif
}
static int lock_state(void){
#ifdef UPDATE_GALLERY_HOST_TEST
    return 0;
#else
    return sceKernelCpuSuspendIntr();
#endif
}
static void unlock_state(int key){
#ifdef UPDATE_GALLERY_HOST_TEST
    (void)key;
#else
    sceKernelCpuResumeIntr(key);
#endif
}
static int safe_id(const char*s){int i;if(!s||!s[0])return 0;for(i=0;s[i];++i){unsigned char c=s[i];if(i>=48||!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return 0;}return strcmp(s,".")&& !strstr(s,"..");}
static int safe_file(const char*s){int i,n=(int)strlen(s);if(n<5||n>40||strcmp(s+n-4,".png"))return 0;for(i=0;i<n-4;i++){unsigned char c=s[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;}return 1;}
static int pair_read(const char*path,char*v,char*r){FILE*f=fopen(path,"rb");int ok=0;char a[51],b[51],tail[4];if(!f)return 0;if(fgets(a,sizeof(a),f)&&strchr(a,'\n')&&fgets(b,sizeof(b),f)&&strchr(b,'\n')){a[strcspn(a,"\r\n")]=0;b[strcspn(b,"\r\n")]=0;ok=safe_id(a)&&safe_id(b)&&!fgets(tail,sizeof(tail),f);if(ok){strcpy(v,a);strcpy(r,b);}}fclose(f);return ok;}
static int pair_write(const char*path,const char*v,const char*r){char tmp[512],old[512];FILE*f;int ok;snprintf(tmp,sizeof(tmp),"%s.new",path);snprintf(old,sizeof(old),"%s.old",path);f=fopen(tmp,"wb");if(!f)return 0;ok=fprintf(f,"%s\n%s\n",v,r)>0;if(fclose(f))ok=0;if(!ok){remove(tmp);return 0;}remove(old);rename(path,old);if(rename(tmp,path)){rename(old,path);return 0;}remove(old);return 1;}
static void release_ui(void){
#ifdef UPDATE_GALLERY_HOST_TEST
    free(memory);
#else
    if(ui_block>=0)sceKernelFreePartitionMemory(ui_block);
    ui_block=-1;
#endif
    memory=NULL;decoded=-1;
}
void update_gallery_shutdown(void){int key=lock_state();++generation;barrier();state=0;unlock_state(key);release_ui();}
void update_gallery_request_for_version(const char*v,const char*r){char a[49],b[49];if(!safe_id(v)||!safe_id(r)||state==2)return;if(pair_read(SEEN,a,b)&&!strcmp(a,v)&&!strcmp(b,r))return;update_gallery_shutdown();snprintf(version,sizeof(version),"%s",v);snprintf(revision,sizeof(revision),"%s",r);pair_write(PENDING,v,r);retry_after=0;barrier();state=1;}
static int load_cached(void);
void update_gallery_load(void){char v[49],r[49];update_gallery_shutdown();if(pair_read(PENDING,v,r)){update_gallery_request_for_version(v,r);if(state==1)load_cached();}}
int update_gallery_pending(void){return state==1&&time(NULL)>=retry_after;}
static void field(void*ud,const char*path,int type,const char*str,long long num){Manifest*m=ud;int i,n=0;char leaf[32];
    if(!strcmp(path,"schema")){if(type!=JSON_NUM||num!=1||m->schema)m->bad=1;else m->schema=1;return;}
    if(!strcmp(path,"version")||!strcmp(path,"revision")){char*dst=!strcmp(path,"version")?m->version:m->revision;if(type!=JSON_STR||!safe_id(str)||dst[0])m->bad=1;else strcpy(dst,str);return;}
    if(sscanf(path,"slides.%d.%31s%n",&i,leaf,&n)==2&&path[n]==0){if(i<0||i>=8){m->bad=1;return;}if(m->count<i+1)m->count=i+1;if(!strcmp(leaf,"file")){if(type!=JSON_STR||!safe_file(str)||(m->files&(1u<<i)))m->bad=1;else{strcpy(m->slides[i].file,str);m->files|=1u<<i;}}else if(!strcmp(leaf,"caption")){if(type!=JSON_STR||strlen(str)>160)m->bad=1;else{size_t k;strcpy(m->slides[i].caption,str);for(k=0;k<strlen(str);k++)if((unsigned char)str[k]<32)m->slides[i].caption[k]=' ';}}else m->bad=1;
    }else m->bad=1;
}
static unsigned be32(const unsigned char*p){return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];}
static int png_header(const unsigned char*p,int n){unsigned w,h;static const unsigned char sig[8]={137,80,78,71,13,10,26,10};if(n<33||memcmp(p,sig,8)||be32(p+8)!=13||memcmp(p+12,"IHDR",4))return 0;w=be32(p+16);h=be32(p+20);return w>0&&w<=480&&h>0&&h<=272;}
static int load_cached(void){
 char text[JSON_MAX+1],path[512];unsigned char header[33];Manifest candidate;FILE*f=fopen(CACHE "index.json","rb");int n;
 if(!f)return 0;
 n=(int)fread(text,1,JSON_MAX+1,f);fclose(f);memset(&candidate,0,sizeof(candidate));
 if(n<1||n>JSON_MAX||json_validate(text,n)<0||json_walk(text,n,field,&candidate)<0||candidate.bad||candidate.schema!=1||strcmp(candidate.version,version)||strcmp(candidate.revision,revision)||candidate.count<1||candidate.files!=((1u<<candidate.count)-1))return 0;
 for(int i=0;i<candidate.count;i++){
  snprintf(path,sizeof(path),CACHE "%d.png",i);f=fopen(path,"rb");if(!f)return 0;
  int got=(int)fread(header,1,sizeof(header),f);int okay=!fseek(f,0,SEEK_END);long size=ftell(f);fclose(f);
  if(!okay||size<33||size>PNG_MAX||!png_header(header,got))return 0;
 }
 gallery=candidate;page=0;decoded=-1;barrier();state=3;return 1;
}
static int download(const char*url,char*buf,int cap,int*size){char err[96];int rc;net_skip_cookie(1);rc=http_get(url,buf,cap,size,6000,err,sizeof(err));net_skip_cookie(0);return rc==0&&*size>0&&*size<=cap-2&&(http_get_expect()<0||http_get_expect()==*size);}
void update_gallery_fetch(void){Manifest next;unsigned char*buf;char url[512],path[512];int i,n=0,ticket,key;int success=0;time_t retry_time;
#ifndef UPDATE_GALLERY_HOST_TEST
    SceUID block;
#endif
    if(!update_gallery_pending())return;
    key=lock_state();if(state!=1){unlock_state(key);return;}state=2;ticket=generation;unlock_state(key);memset(&next,0,sizeof(next));
#ifdef UPDATE_GALLERY_HOST_TEST
    buf=malloc(PNG_MAX+2);
#ifdef _WIN32
    _mkdir(CACHE);
#else
    mkdir(CACHE,0777);
#endif
#else
    block=sceKernelAllocPartitionMemory(2,"update_fetch",PSP_SMEM_High,PNG_MAX+2,NULL);buf=block>=0?sceKernelGetBlockHeadAddr(block):NULL;sceIoMkdir(CACHE,0777);
#endif
    if(!buf)goto done;
    snprintf(url,sizeof(url),BASE "%s/index.json",version);
    if(!download(url,(char*)buf,JSON_MAX+2,&n)||json_validate((char*)buf,n)<0||json_walk((char*)buf,n,field,&next)<0||next.bad||next.schema!=1||strcmp(next.version,version)||strcmp(next.revision,revision)||next.count<1||next.files!=((1u<<next.count)-1))goto done;
    for(i=0;i<next.count;i++){FILE*f;int wrote;snprintf(url,sizeof(url),BASE "%s/%s",version,next.slides[i].file);if(ticket!=generation||!download(url,(char*)buf,PNG_MAX+2,&n)||!png_header(buf,n))goto done;snprintf(path,sizeof(path),CACHE "%d.png",i);f=fopen(path,"wb");if(!f)goto done;wrote=fwrite(buf,1,n,f)==(size_t)n;if(fclose(f))wrote=0;if(!wrote)goto done;}
    success=1;
done:
#ifdef UPDATE_GALLERY_HOST_TEST
    free(buf);
#else
    if(block>=0)sceKernelFreePartitionMemory(block);
#endif
    retry_time=time(NULL)+60;
    key=lock_state();
    if(ticket==generation&&state==2){if(success){gallery=next;page=0;decoded=-1;barrier();state=3;}else{retry_after=retry_time;barrier();state=1;}}
    unlock_state(key);
}
static int decode_page(void){
#ifdef UPDATE_GALLERY_HOST_TEST
    decoded=page;width=480;height=240;return 1;
#else
    char path[512];FILE*f;int n,k;unsigned char*rgba;
    if(decoded==page)return 1;
    if(!memory){ui_free_skin_preview();ui_block=sceKernelAllocPartitionMemory(2,"update_gallery",PSP_SMEM_High,UI_BYTES,NULL);if(ui_block<0)return 0;memory=sceKernelGetBlockHeadAddr(ui_block);if(!memory){release_ui();return 0;}}
    snprintf(path,sizeof(path),CACHE "%d.png",page);f=fopen(path,"rb");if(!f)return 0;n=fread(memory,1,PNG_MAX,f);fclose(f);if(!png_header(memory,n))return 0;rgba=memory+PNG_MAX;
    if(png_decode_rgba_scaled(memory,n,rgba,480,&width,&height)<0||width<1||height<1||width>480||height>272)return 0;
    {unsigned short*dst=(unsigned short*)(rgba+PIXELS*4);for(k=0;k<width*height;k++){int a=rgba[k*4+3];dst[k]=gfx_rgb(rgba[k*4]*a/255,rgba[k*4+1]*a/255,rgba[k*4+2]*a/255);}}
    decoded=page;return 1;
#endif
}
int update_gallery_active(void){if(state!=3&&state!=4)return 0;barrier();if(!decode_page()){update_gallery_shutdown();return 0;}state=4;return 1;}
static void close_gallery(int seen){if(seen&&pair_write(SEEN,version,revision))remove(PENDING);update_gallery_shutdown();}
int update_gallery_input(unsigned pressed){if(!update_gallery_active())return 0;if(pressed&0x2000u){close_gallery(page==gallery.count-1);return 1;}if(pressed&(0x20u|0x4000u)){if(page+1>=gallery.count)close_gallery(1);else{++page;decoded=-1;}}else if((pressed&0x80u)&&page>0){--page;decoded=-1;}return 1;}
void update_gallery_draw(float time){(void)time;if(!update_gallery_active())return;
#ifndef UPDATE_GALLERY_HOST_TEST
    {int w=width,h=height;char footer[96];unsigned short*px=(unsigned short*)(memory+PNG_MAX+PIXELS*4);if(h>240){w=w*240/h;h=240;}gfx_clear(0);gfx_blit565_scale((480-w)/2,(240-h)/2,w,h,px,width,height);gfx_set_clip(8,240,472,256);gfx_text_center(FS_SM,8,464,241,gallery.slides[page].caption,gfx_rgb(235,235,235));gfx_reset_clip();snprintf(footer,sizeof(footer),"%d / %d   < / >   %s",page+1,gallery.count,page+1==gallery.count?"X / > Finish":"X Next   O Close");gfx_text_center(FS_SM,0,480,258,footer,gfx_rgb(170,170,170));}
#endif
}
