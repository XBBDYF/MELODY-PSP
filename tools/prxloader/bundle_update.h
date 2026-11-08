#ifndef MELODY_BUNDLE_UPDATE_H
#define MELODY_BUNDLE_UPDATE_H
#include <zlib.h>
#ifndef MB_PROGRESS
#define MB_PROGRESS(stage,name,percent) ((void)0)
#endif
/* DATA.PSAR bundle: validated in full before any installed module is replaced.
 * A journal restores the previous modules if power is lost during commit. */
#define MB_ROOT "./"
#define MB_MARK MB_ROOT "update34-installed.bin"
#define MB_JOURNAL MB_ROOT "update34-pending.bin"
static const char *const mb_names[]={"melody.prx","melody_mv565.prx","melody_mv565_fast.prx","melody_mv565_balanced.prx","melody_mv565_sharp.prx","melody_mv_avc.prx","avc_firmware_load.prx","avc_profile_boot.prx"};
typedef struct {char magic[8];unsigned version,count,bytes,crc,reserved[2];} MbHeader;
typedef struct {char name[56];unsigned bytes,crc,offset,reserved;} MbEntry;
static unsigned char mb_buffer[16384],mb_output[16384];
static unsigned mb_crc(unsigned crc,const void*data,unsigned bytes){const unsigned char*p=(const unsigned char*)data;for(unsigned i=0;i<bytes;++i){crc^=p[i];for(unsigned k=0;k<8;++k)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}return crc;}
static int mb_read(int fd,void*data,unsigned bytes){unsigned at=0;while(at<bytes){int n=sceIoRead(fd,(unsigned char*)data+at,bytes-at);if(n<=0||(unsigned)n>bytes-at)return -1;at+=(unsigned)n;}return 0;}
static int mb_write(int fd,const void*data,unsigned bytes){unsigned at=0;while(at<bytes){int n=sceIoWrite(fd,(const unsigned char*)data+at,bytes-at);if(n<=0||(unsigned)n>bytes-at)return -1;at+=(unsigned)n;}return 0;}
static void mb_path(char*out,unsigned size,const char*name,const char*suffix){snprintf(out,size,MB_ROOT "%s%s",name,suffix);}
static int mb_store(const char*path,const unsigned*data,unsigned count){int f=sceIoOpen(path,PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0600);if(f<0)return -1;int r=mb_write(f,data,count*4u);if(sceIoClose(f)<0)r=-1;if(r==0&&sceIoSync("ms0:",0)<0)r=-1;return r;}
static int mb_recover(unsigned installed){
 unsigned journal[3];int f=sceIoOpen(MB_JOURNAL,PSP_O_RDONLY,0);if(f<0)return 0;
 int valid=mb_read(f,journal,sizeof(journal))==0;sceIoClose(f);
 if(!valid||journal[0]!=0x4d424a31u||(journal[2]&~255u))return -1;
 MB_PROGRESS("Recovering previous update","",0);
 if(journal[1]==installed){return sceIoRemove(MB_JOURNAL)<0?-1:0;}
 for(unsigned i=0;i<8;++i){char live[160],old[160];SceIoStat st;mb_path(live,sizeof(live),mb_names[i],"");mb_path(old,sizeof(old),mb_names[i],".bak34");
  if(journal[2]&(1u<<i)){
   if(sceIoGetstat(old,&st)>=0){if(sceIoGetstat(live,&st)>=0&&sceIoRemove(live)<0)return -1;if(sceIoRename(old,live)<0)return -1;}
  }else if(sceIoGetstat(live,&st)>=0&&sceIoRemove(live)<0)return -1;
 }
 return sceIoRemove(MB_JOURNAL)<0?-1:0;
}
static int mb_install(void){
 unsigned installed=0;int f=sceIoOpen(MB_MARK,PSP_O_RDONLY,0);if(f>=0){if(mb_read(f,&installed,4)<0)installed=0;sceIoClose(f);}
 if(mb_recover(installed)<0)return -1;
 int input=sceIoOpen(MB_ROOT "EBOOT.PBP",PSP_O_RDONLY,0);if(input<0)return -1;
 unsigned pbp[10];MbHeader h;MbEntry entries[8];int result=-1;unsigned base=0,total_output=0,done_output=0;
 if(mb_read(input,pbp,sizeof(pbp))<0||pbp[0]!=0x50425000u)goto finish;
 base=pbp[9];
 {SceOff end=sceIoLseek(input,0,PSP_SEEK_END);if(end<0||base>(unsigned long long)end)goto finish;if((unsigned long long)end-base==0){result=0;goto finish;}
  if(sceIoLseek(input,base,PSP_SEEK_SET)<0||mb_read(input,&h,sizeof(h))<0)goto finish;
  if(memcmp(h.magic,"MLDBNDL1",8)||(h.version!=1&&h.version!=2)||h.count!=8||h.reserved[0]||h.reserved[1]||h.bytes<sizeof(h)+sizeof(entries)||h.bytes>48u*1024u*1024u||(unsigned long long)h.bytes!=(unsigned long long)end-base)goto finish;}
 if(mb_read(input,entries,sizeof(entries))<0)goto finish;
 {unsigned next=sizeof(h)+sizeof(entries);
  for(unsigned i=0;i<8;++i){unsigned stored=h.version==2?entries[i].reserved:entries[i].bytes;
   if(memchr(entries[i].name,0,56)==NULL||strcmp(entries[i].name,mb_names[i])||(h.version==1&&entries[i].reserved)||entries[i].offset!=next||!entries[i].bytes||entries[i].bytes>16u*1024u*1024u||!stored||stored>h.bytes-next)goto finish;next+=stored;total_output+=entries[i].bytes;}
  if(next!=h.bytes)goto finish;}
 if(installed==h.crc){result=0;goto finish;}
 MB_PROGRESS("Verifying update","",0);
 if(sceIoLseek(input,base+sizeof(h),PSP_SEEK_SET)<0)goto finish;
 {unsigned crc=0xffffffffu,left=h.bytes-sizeof(h);while(left){unsigned n=left>sizeof(mb_buffer)?sizeof(mb_buffer):left;if(mb_read(input,mb_buffer,n)<0)goto finish;crc=mb_crc(crc,mb_buffer,n);left-=n;MB_PROGRESS("Verifying update","",(unsigned)((unsigned long long)(h.bytes-sizeof(h)-left)*20/(h.bytes-sizeof(h))));}if((crc^0xffffffffu)!=h.crc)goto finish;}
 /* Stage every file. A failed write or CRC leaves all live modules unchanged. */
 for(unsigned i=0;i<8;++i){MB_PROGRESS("Extracting modules",mb_names[i],20+(unsigned)((unsigned long long)done_output*70/total_output));char stage[160];mb_path(stage,sizeof(stage),mb_names[i],".upd34");int out=sceIoOpen(stage,PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0600);if(out<0)goto finish;
  unsigned crc=0xffffffffu,left=h.version==2?entries[i].reserved:entries[i].bytes,produced=0;int okay=sceIoLseek(input,base+entries[i].offset,PSP_SEEK_SET)>=0;
  if(h.version==1){
   while(okay&&left){unsigned n=left>sizeof(mb_buffer)?sizeof(mb_buffer):left;if(mb_read(input,mb_buffer,n)<0||mb_write(out,mb_buffer,n)<0){okay=0;break;}crc=mb_crc(crc,mb_buffer,n);left-=n;produced+=n;MB_PROGRESS("Extracting modules",mb_names[i],20+(unsigned)((unsigned long long)(done_output+produced)*70/total_output));}
  }else{
   z_stream z;memset(&z,0,sizeof(z));int initialized=inflateInit(&z)==Z_OK,status=Z_OK;
   if(!initialized)okay=0;
   while(okay&&status!=Z_STREAM_END){
    if(z.avail_in==0&&left){unsigned n=left>sizeof(mb_buffer)?sizeof(mb_buffer):left;if(mb_read(input,mb_buffer,n)<0){okay=0;break;}z.next_in=mb_buffer;z.avail_in=n;left-=n;}
    unsigned before=z.avail_in;z.next_out=mb_output;z.avail_out=sizeof(mb_output);status=inflate(&z,Z_NO_FLUSH);unsigned n=sizeof(mb_output)-z.avail_out;
    if((status!=Z_OK&&status!=Z_STREAM_END)||n>entries[i].bytes-produced){okay=0;break;}
    if(n){if(mb_write(out,mb_output,n)<0){okay=0;break;}crc=mb_crc(crc,mb_output,n);produced+=n;MB_PROGRESS("Extracting modules",mb_names[i],20+(unsigned)((unsigned long long)(done_output+produced)*70/total_output));}
    if(status!=Z_STREAM_END&&before==z.avail_in&&!n){okay=0;break;}
   }
   if(initialized){if(status!=Z_STREAM_END||left||z.avail_in)okay=0;inflateEnd(&z);}
  }
  if(sceIoClose(out)<0)okay=0;if(!okay||produced!=entries[i].bytes||(crc^0xffffffffu)!=entries[i].crc)goto finish;
  done_output+=produced;
 }
 if(sceIoSync("ms0:",0)<0)goto finish;
 {unsigned mask=0;SceIoStat st;
  for(unsigned i=0;i<8;++i){char live[160],old[160];mb_path(live,sizeof(live),mb_names[i],"");mb_path(old,sizeof(old),mb_names[i],".bak34");if(sceIoGetstat(live,&st)>=0)mask|=1u<<i;
   if(sceIoGetstat(old,&st)>=0&&sceIoRemove(old)<0)goto finish;}
  unsigned journal[3]={0x4d424a31u,h.crc,mask};if(mb_store(MB_JOURNAL,journal,3)<0)goto finish;
  for(unsigned i=0;i<8;++i){MB_PROGRESS("Installing modules",mb_names[i],90+i);char live[160],old[160],stage[160];mb_path(live,sizeof(live),mb_names[i],"");mb_path(old,sizeof(old),mb_names[i],".bak34");mb_path(stage,sizeof(stage),mb_names[i],".upd34");
   if((mask&(1u<<i))&&sceIoRename(live,old)<0){mb_recover(installed);goto finish;}
   if(sceIoRename(stage,live)<0){mb_recover(installed);goto finish;}
  }
  if(mb_store(MB_MARK,&h.crc,1)<0){mb_recover(installed);mb_store(MB_MARK,&installed,1);goto finish;}
  if(sceIoRemove(MB_JOURNAL)<0)goto finish;
 }
 MB_PROGRESS("Complete - starting MELODY","",100);
 result=1;
 finish:sceIoClose(input);return result;
}
#endif
