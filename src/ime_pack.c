/* Data only: no PSP/plugin ABI, no global active dictionary, no worker/UI sharing. */
#include "ime_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ime_font_data.h"
#ifdef __PSP__
#include <pspiofilemgr.h>
#include <pspsysmem.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif
#ifdef __PSP__
typedef SceUID PackFile;
#define PACK_INVALID (-1)
#else
typedef FILE *PackFile;
#define PACK_INVALID NULL
#endif
struct ImePack {
 PackFile file;
 unsigned char *snapshot;
 char path[512];
 unsigned int io_reads, record_reads;
#ifdef __PSP__
 SceUID block;
#endif
 uint32_t counts[3], starts[3], revision, total;
};
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint32_t crc_step(uint32_t crc,const unsigned char *p,size_t n) {
 while(n--) { crc^=*p++; for(int i=0;i<8;i++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u); } return crc;
}
static PackFile open_read(const char *path) {
#ifdef __PSP__
 return sceIoOpen(path,PSP_O_RDONLY,0);
#elif defined(_WIN32)
 HANDLE h=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
 if(h==INVALID_HANDLE_VALUE) return NULL;
 int fd=_open_osfhandle((intptr_t)h,_O_RDONLY|_O_BINARY);
 if(fd<0) { CloseHandle(h); return NULL; }
 FILE *f=_fdopen(fd,"rb"); if(!f) _close(fd); return f;
#else
 return fopen(path,"rb");
#endif
}
/* PSP never touches the shared newlib FILE list. Each reader owns its descriptor.
 * SDK calls are bounded to a page/copy block, without the global stdio lock. */
static PackFile open_write(const char *path) {
#ifdef __PSP__
 return sceIoOpen(path,PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0666);
#else
 return fopen(path,"wb");
#endif
}
static int file_read(PackFile f,void *out,unsigned int size) {
#ifdef __PSP__
 return sceIoRead(f,out,size);
#else
 size_t n=fread(out,1,size,f);return ferror(f)?-1:(int)n;
#endif
}
static int file_write(PackFile f,const void *in,unsigned int size) {
#ifdef __PSP__
 return sceIoWrite(f,in,size);
#else
 return (int)fwrite(in,1,size,f);
#endif
}
static int file_close(PackFile f) {
#ifdef __PSP__
 return sceIoClose(f);
#else
 return fclose(f);
#endif
}
static long file_seek(PackFile f,long offset,int whence) {
#ifdef __PSP__
 return (long)sceIoLseek32(f,(int)offset,whence);
#else
 if(fseek(f,offset,whence))return -1;
 return ftell(f);
#endif
}
static int file_publish(const char *temp,const char *current) {
#ifdef __PSP__
 return sceIoRename(temp,current)<0?IME_PACK_IO:0;
#elif defined(_WIN32)
 return ReplaceFileA(current,temp,NULL,0,NULL,NULL)||
        MoveFileExA(temp,current,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)?0:IME_PACK_IO;
#else
 return rename(temp,current)?IME_PACK_IO:0;
#endif
}
static void file_remove(const char *path) {
#ifdef __PSP__
 sceIoRemove(path);
#else
 remove(path);
#endif
}
static int raw_record(ImePack *p,uint32_t index,unsigned char out[80]) {
 if(index>=p->total||!p->snapshot)return IME_PACK_BOUNDS;
 p->record_reads++;
 memcpy(out,p->snapshot+index*80,80);return 0;
}
static int font_has(unsigned int cp) {
 unsigned int a=0,b=(unsigned int)(sizeof(k_ime_font_cps)/sizeof(k_ime_font_cps[0]));
 if(cp>=32&&cp<127) return 1;
 while(a<b) { unsigned int m=a+(b-a)/2; if(k_ime_font_cps[m]<cp) a=m+1; else b=m; }
 return a<sizeof(k_ime_font_cps)/sizeof(k_ime_font_cps[0])&&k_ime_font_cps[a]==cp;
}
/* Strict UTF-8; reject overlongs, surrogates, controls and glyphs absent from fallback font. */
static int text_valid(const char *s,int font) {
 const unsigned char *p=(const unsigned char*)s;
 while(*p) {
  unsigned int cp; int n;
  if(*p<128) { cp=*p; n=1; }
  else if(*p>=0xc2&&*p<=0xdf) { cp=*p&31; n=2; }
  else if(*p>=0xe0&&*p<=0xef) { cp=*p&15; n=3; }
  else if(*p>=0xf0&&*p<=0xf4) { cp=*p&7; n=4; }
  else return 0;
  for(int i=1;i<n;i++) { if((p[i]&0xc0)!=0x80) return 0; cp=(cp<<6)|(p[i]&63); }
  if((n==2&&cp<128)||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||cp<32) return 0;
  if(font&&!font_has(cp)) return 0;
  p+=n;
 }
 return 1;
}
static int decode(const unsigned char *b,ImePackEntry *e) {
 memcpy(e->key,b,48); memcpy(e->word,b+48,24); e->freq=le32(b+72); e->kind=le32(b+76);
 if(!memchr(e->key,0,48)||!memchr(e->word,0,24)||!e->key[0]||!e->word[0]) return IME_PACK_BOUNDS;
 /* Padding must be zero, so each record has a single canonical representation. */
 for(size_t i=strlen(e->key)+1;i<48;i++) if(e->key[i]) return IME_PACK_FORMAT_ERROR;
 for(size_t i=strlen(e->word)+1;i<24;i++) if(e->word[i]) return IME_PACK_FORMAT_ERROR;
 if(!text_valid(e->key,0)||!text_valid(e->word,0)) return IME_PACK_FORMAT_ERROR;
 if(!text_valid(e->word,1)) return IME_PACK_FONT;
 if(e->kind>2||!e->freq) return IME_PACK_FORMAT_ERROR;
 if(e->kind!=IME_PACK_KANA) for(const char *p=e->key;*p;p++) if(*p<'a'||*p>'z') return IME_PACK_FORMAT_ERROR;
 if(e->kind==IME_PACK_CHAR) {
  int n=1; unsigned char c=(unsigned char)e->word[0];
  if(c>=0xf0) n=4; else if(c>=0xe0) n=3; else if(c>=0xc0) n=2;
  if((int)strlen(e->word)!=n) return IME_PACK_FORMAT_ERROR;
 }
 return 0;
}
int ime_pack_prepare(const char *path,ImePack **out) {
 unsigned char h[64],b[80]; ImePackEntry prev,e; uint32_t crc=~0u,n,counts[3]={0,0,0}; int err=IME_PACK_IO; ImePack *p=NULL;
 if(!out) return IME_PACK_FORMAT_ERROR;
 *out=NULL; if(!path) return IME_PACK_IO;
 PackFile f=open_read(path); if(f==PACK_INVALID) return IME_PACK_IO;
 if(file_read(f,h,64)!=64) goto fail;
 err=IME_PACK_FORMAT_ERROR;
 if(memcmp(h,"MIMEPK1\0",8)||le32(h+8)!=1||le32(h+12)!=64||le32(h+40)!=80) goto fail;
 for(int i=48;i<64;i++) if(h[i]) goto fail;
 n=le32(h+24); err=IME_PACK_BOUNDS;
 if(!n||n>IME_PACK_MAX_RECORDS||le32(h+16)!=64+n*80) goto fail;
 if(le32(h+28)>n||le32(h+32)>n||le32(h+36)>n||le32(h+28)+le32(h+32)+le32(h+36)!=n) goto fail;
 if(file_seek(f,0,SEEK_END)!=(long)(64+n*80)) goto fail;
 p=(ImePack*)calloc(1,sizeof(*p));err=IME_PACK_MEMORY;if(!p)goto fail;
 p->file=f;p->total=n;
#ifdef __PSP__
 p->block=-1;
#endif
 if(strlen(path)>=sizeof(p->path)) { err=IME_PACK_BOUNDS;goto fail; }
 strcpy(p->path,path);
#ifdef __PSP__
 if(sceKernelMaxFreeMemSize()<n*80u+1024u*1024u) { err=IME_PACK_MEMORY;goto fail; }
 p->block=sceKernelAllocPartitionMemory(2,"ime_snapshot",PSP_SMEM_High,n*80u,NULL);
 if(p->block>=0)p->snapshot=(unsigned char*)sceKernelGetBlockHeadAddr(p->block);
#else
 p->snapshot=(unsigned char*)malloc(n*80u);
#endif
 if(!p->snapshot) { err=IME_PACK_MEMORY;goto fail; }
 if(file_seek(f,64,SEEK_SET)<0) { err=IME_PACK_IO;goto fail; }
 for(unsigned int done=0;done<n*80u;) {
  unsigned int bytes=n*80u-done;if(bytes>32768u)bytes=32768u;
  p->io_reads++;int got=file_read(f,p->snapshot+done,bytes);
  if(got<=0) { err=IME_PACK_IO;goto fail; }
  done+=(unsigned int)got;
 }

 memset(&prev,0,sizeof(prev));
 for(uint32_t i=0;i<n;i++) {
  err=raw_record(p,i,b);if(err)goto fail;
  crc=crc_step(crc,b,80); err=decode(b,&e); if(err) goto fail;
  err=IME_PACK_FORMAT_ERROR;
  if(i&&(e.kind<prev.kind||(e.kind==prev.kind&&(strcmp(e.key,prev.key)<0||(!strcmp(e.key,prev.key)&&(e.freq>prev.freq||(e.freq==prev.freq&&strcmp(e.word,prev.word)<=0))))))) goto fail;
  counts[e.kind]++; prev=e;
 }
 err=IME_PACK_CRC; if((crc^~0u)!=le32(h+20)) goto fail;
 err=IME_PACK_FORMAT_ERROR; for(int i=0;i<3;i++) if(counts[i]!=le32(h+28+i*4)) goto fail;
 p->file=f; memcpy(p->counts,counts,sizeof(counts)); p->starts[1]=counts[0];p->starts[2]=counts[0]+counts[1];p->revision=le32(h+44);
 file_close(f);p->file=PACK_INVALID;
 *out=p; return 0;
fail: if(p)ime_pack_close(p);else file_close(f);return err;
}
void ime_pack_close(ImePack *p) { if(p) {
 if(p->file!=PACK_INVALID)file_close(p->file);
#ifdef __PSP__
 if(p->block>=0)sceKernelFreePartitionMemory(p->block);
#else
 free(p->snapshot);
#endif
 free(p);
} }
const char *ime_pack_path(const ImePack *p) { return p?p->path:""; }
unsigned int ime_pack_io_reads(const ImePack *p) { return p?p->io_reads:0; }
unsigned int ime_pack_record_reads(const ImePack *p) { return p?p->record_reads:0; }
int ime_pack_validate(const char *path) { ImePack *p=NULL; int r=ime_pack_prepare(path,&p); ime_pack_close(p); return r; }
unsigned int ime_pack_count(const ImePack *p,int kind) { return p&&kind>=0&&kind<3?p->counts[kind]:0; }
unsigned int ime_pack_revision(const ImePack *p) { return p?p->revision:0; }
int ime_pack_read(ImePack *p,int kind,unsigned int index,ImePackEntry *out) {
 unsigned char b[80]; if(!p||!out||kind<0||kind>2||index>=p->counts[kind]) return IME_PACK_BOUNDS;
 int r=raw_record(p,p->starts[kind]+index,b);if(r)return r;
 /* The activated inode is immutable. Recheck record boundaries on read nevertheless. */
 memcpy(out->key,b,48);memcpy(out->word,b+48,24);out->freq=le32(b+72);out->kind=le32(b+76);return 0;
}
int ime_pack_install(const char *source,const char *current) {
 char temp[512];unsigned char b[4096];int n,err;
 if(!source||!current||!strcmp(source,current)||snprintf(temp,sizeof(temp),"%s.install",current)>=(int)sizeof(temp)||!strcmp(source,temp))return IME_PACK_BOUNDS;
 err=ime_pack_validate(source);if(err)return err;
 PackFile in=open_read(source);if(in==PACK_INVALID)return IME_PACK_IO;
 PackFile out=open_write(temp);if(out==PACK_INVALID){file_close(in);return IME_PACK_IO;}
 err=0;
 while((n=file_read(in,b,sizeof(b)))>0) {
  unsigned int done=0;
  while(done<(unsigned int)n) {
   int wrote=file_write(out,b+done,(unsigned int)n-done);
   if(wrote<=0){err=IME_PACK_IO;break;}
   done+=(unsigned int)wrote;
  }
  if(err)break;
 }
 if(n<0)err=IME_PACK_IO;
 if(file_close(out)<0)err=IME_PACK_IO;
 file_close(in);
 if(!err)err=ime_pack_validate(temp);
 if(!err)err=file_publish(temp,current);
 if(err)file_remove(temp);
 return err;
}
