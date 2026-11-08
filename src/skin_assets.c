#include "skin_assets.h"
#include "json.h"
#include "png.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <zlib.h>
#define IMAGE_MAX 12
#define FILE_MAX 131072
#define IMAGE_DIM 512
#define SLOT_BYTES (IMAGE_DIM*IMAGE_DIM*4)
#define RAW_BYTES ((IMAGE_DIM*4+1)*IMAGE_DIM)
#define WORK_BYTES (FILE_MAX+2*SLOT_BYTES+RAW_BYTES)
typedef struct{char id[33],file[96],type[16];unsigned fields;}ImageDecl;
typedef struct{ImageDecl entries[20];int count,bad;}Manifest;
typedef struct{char id[33];int w,h,valid;unsigned age;}ImageSlot;
static char g_root[256];static Manifest g_manifest;static ImageSlot g_slots[2];static unsigned g_age,g_decode_us;static int g_decoded;
static SceUID g_block=-1;static unsigned char*g_memory;
static int safe_id(const char*s){size_t n=strlen(s);if(!n||n>32)return 0;for(size_t i=0;i<n;++i)if(!((s[i]>='A'&&s[i]<='Z')||(s[i]>='a'&&s[i]<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'))return 0;return 1;}
static int safe_file(const char*s,const char*ext){size_t n=strlen(s);if(n<12||n>=96||strncmp(s,"assets/",7)||strcmp(s+n-4,ext))return 0;char id[40];size_t len=n-11;if(len>=sizeof(id))return 0;memcpy(id,s+7,len);id[len]=0;return safe_id(id);}
static void manifest_cb(void*ud,const char*path,int type,const char*text,long long num){(void)num;Manifest*m=ud;const char*seg[5];int len[5],n=json_split_path(path,seg,len,5);if(n<1||len[0]!=6||strncmp(seg[0],"assets",6))return;if(n==1){m->bad=1;return;}if(n==2){int i=atoi(seg[1]);if(i<0||i>=20){m->bad=1;return;}if(i+1>m->count)m->count=i+1;return;}if(n!=3||type!=JSON_STR||!text){m->bad=1;return;}int i=atoi(seg[1]);if(i<0||i>=20){m->bad=1;return;}if(i+1>m->count)m->count=i+1;ImageDecl*d=&m->entries[i];char*out=NULL;int cap=0,field=0;if(len[2]==2&&!strncmp(seg[2],"id",2)){out=d->id;cap=sizeof(d->id);field=1;}else if(len[2]==4&&!strncmp(seg[2],"file",4)){out=d->file;cap=sizeof(d->file);field=2;}else if(len[2]==4&&!strncmp(seg[2],"type",4)){out=d->type;cap=sizeof(d->type);field=4;}else{m->bad=1;return;}if((d->fields&field)||strlen(text)>=(size_t)cap){m->bad=1;return;}d->fields|=field;snprintf(out,cap,"%s",text);}
static int parse(const char*text,Manifest*out,char*err,int cap){memset(out,0,sizeof(*out));const char*assets=strstr(text,"\"assets\"");if(assets){if(strstr(assets+8,"\"assets\"")){snprintf(err,cap,"duplicate assets declaration");return 0;}const char*p=assets+8;while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')++p;if(*p++!=':'){snprintf(err,cap,"invalid assets field");return 0;}while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')++p;if(*p!='['){snprintf(err,cap,"assets must be an array");return 0;}}if(json_walk(text,strlen(text),manifest_cb,out)<0||out->bad){snprintf(err,cap,"invalid skin assets");return 0;}if(assets){const char*p=strchr(assets,'[');int depth=0,string=0,escaped=0,elements=0;if(p)for(++p;*p;++p){char ch=*p;if(string){if(escaped)escaped=0;else if(ch=='\\')escaped=1;else if(ch=='\"')string=0;continue;}if(ch=='\"'){string=1;continue;}if(ch=='{'&&depth++==0)++elements;else if(ch=='}')--depth;else if(ch==']'&&depth==0)break;}if(elements!=out->count){snprintf(err,cap,"empty or malformed resource entry");return 0;}}
 int images=0,sounds=0;for(int i=0;i<out->count;++i){ImageDecl*d=&out->entries[i];int image=!strcmp(d->type,"image"),sound=!strcmp(d->type,"sound");if(d->fields!=7||!safe_id(d->id)||(!image&&!sound)||!safe_file(d->file,image?".png":".wav")){snprintf(err,cap,"invalid asset id/file/type");return 0;}images+=image;sounds+=sound;for(int j=0;j<i;++j)if(!strcasecmp(d->id,out->entries[j].id)||!strcasecmp(d->file,out->entries[j].file)){snprintf(err,cap,"duplicate asset id/file");return 0;}}if(images>12||sounds>8){snprintf(err,cap,"asset count limit");return 0;}return 1;}
static unsigned read32(const unsigned char*p){return((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];}
static int read_local(const char*root,const char*file,unsigned char*data,int max){char path[368];if(snprintf(path,sizeof(path),"%s/%s",root,file)>=(int)sizeof(path))return -1;int fd=sceIoOpen(path,PSP_O_RDONLY,0);if(fd<0)return -1;long long size=sceIoLseek(fd,0,PSP_SEEK_END);if(size<=0||size>max||sceIoLseek(fd,0,PSP_SEEK_SET)<0){sceIoClose(fd);return -1;}int got=0;while(got<size){int n=sceIoRead(fd,data+got,(int)size-got);if(n<=0)break;got+=n;}sceIoClose(fd);return got==size?got:-1;}
static int png_valid(const unsigned char*p,int size){if(size<33||memcmp(p,"\x89PNG\r\n\x1a\n",8)||read32(p+8)!=13||memcmp(p+12,"IHDR",4)||!read32(p+16)||!read32(p+20)||read32(p+16)>512||read32(p+20)>512||p[24]!=8||(p[25]!=0&&p[25]!=2&&p[25]!=3&&p[25]!=6)||p[26]||p[27]||p[28])return 0;int at=8,ended=0;while(at<=size-12){unsigned n=read32(p+at);if(n>(unsigned)(size-at-12))return 0;unsigned crc=crc32(0L,Z_NULL,0);crc=crc32(crc,p+at+4,n+4);if(read32(p+at+8+n)!=crc)return 0;if(!memcmp(p+at+4,"IEND",4)){ended=n==0;break;}at+=n+12;}return ended;}
int skin_assets_validate_manifest(const char*root,const char*text,char*err,int cap){Manifest m;if(!root||strlen(root)>=256||!text||strlen(text)>24576)return 0;return parse(text,&m,err,cap);}
int skin_assets_probe(const char*root,char*err,int cap){unsigned char*text=malloc(24577);if(!text){snprintf(err,cap,"asset metadata memory unavailable");return 0;}int n=read_local(root,"skin.json",text,24576);if(n<0){free(text);snprintf(err,cap,"missing skin metadata");return 0;}text[n]=0;Manifest m;int result=parse((char*)text,&m,err,cap);free(text);if(!result)return 0;if(!m.count)return 1;unsigned char*buffer=malloc(FILE_MAX);if(!buffer){snprintf(err,cap,"asset check memory unavailable");return 0;}for(int i=0;i<m.count;++i){ImageDecl*d=&m.entries[i];n=read_local(root,d->file,buffer,!strcmp(d->type,"image")?FILE_MAX:65536);if(n<0||(!strcmp(d->type,"image")&&!png_valid(buffer,n))){free(buffer);snprintf(err,cap,"invalid or missing asset %s",d->id);return 0;}}free(buffer);return 1;}
void skin_assets_release(void){if(g_block>=0)sceKernelFreePartitionMemory(g_block);g_block=-1;g_memory=NULL;g_root[0]=0;memset(&g_manifest,0,sizeof(g_manifest));memset(g_slots,0,sizeof(g_slots));g_decoded=0;}
void skin_assets_set_root(const char*root){skin_assets_release();if(!root||strlen(root)>=sizeof(g_root))return;unsigned char*text=malloc(24577);if(!text)return;int n=read_local(root,"skin.json",text,24576);char err[80];if(n>0){text[n]=0;if(parse((char*)text,&g_manifest,err,sizeof(err)))snprintf(g_root,sizeof(g_root),"%s",root);}free(text);}
void skin_assets_begin_frame(void){g_decoded=0;}
int skin_assets_image(const char*id,int x,int y,int w,int h,int alpha){g_decode_us=0;if(!id||!safe_id(id)||x< -480||y< -272||x>960||y>544||w<0||h<0||w>960||h>544||alpha<0||alpha>255)return -1;if(!w||!h||!alpha)return 0;int entry=-1;for(int i=0;i<g_manifest.count;++i)if(!strcmp(g_manifest.entries[i].id,id)&&!strcmp(g_manifest.entries[i].type,"image")){entry=i;break;}if(entry<0)return 0;
 int slot=-1;for(int i=0;i<2;++i)if(!strcmp(g_slots[i].id,id)){slot=i;break;}
 if(slot<0){if(g_decoded)return 0;g_decoded=1;if(!g_memory){g_block=sceKernelAllocPartitionMemory(2,"skin_images",PSP_SMEM_High,WORK_BYTES,NULL);if(g_block<0)return -1;g_memory=sceKernelGetBlockHeadAddr(g_block);if(!g_memory){sceKernelFreePartitionMemory(g_block);g_block=-1;return -1;}}
  unsigned started=sceKernelGetSystemTimeLow();slot=g_slots[0].age<=g_slots[1].age?0:1;ImageSlot*s=&g_slots[slot];memset(s,0,sizeof(*s));snprintf(s->id,sizeof(s->id),"%s",id);int n=read_local(g_root,g_manifest.entries[entry].file,g_memory,FILE_MAX);if(n>0&&png_valid(g_memory,n)&&png_decode_rgba_workspace(g_memory,n,g_memory+FILE_MAX+slot*SLOT_BYTES,SLOT_BYTES,&s->w,&s->h,g_memory+FILE_MAX+2*SLOT_BYTES,RAW_BYTES)==0)s->valid=1;g_decode_us=sceKernelGetSystemTimeLow()-started;
 }
 ImageSlot*s=&g_slots[slot];s->age=++g_age;if(!s->valid)return -1;gfx_blit_rgba_scaled(x,y,w,h,g_memory+FILE_MAX+slot*SLOT_BYTES,s->w,s->h,alpha);return 1;
}

unsigned skin_assets_decode_us(void){return g_decode_us;}

unsigned skin_assets_memory_bytes(void){return g_memory?WORK_BYTES:0;}
