"""Production read callback must not depend on the video PRX thread cwd."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];s=(R/'src/mv.c').read_text(encoding='utf-8')
def fn(sig):
 a=s.index(sig);end=s.index('\n}',a)+2;return s[a:end]+'\n'
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mv_stream_contract.h"
typedef int SceUID;typedef long long SceOff;
#define PSP_O_RDONLY 1
#define PSP_SEEK_SET SEEK_SET
#define mlog(...) ((void)0)
typedef struct{long long available,total;int done,error,cancel;}MvSourceSnapshot;
static char g_src_final[512],g_src_part[512],root[512];static int g_src_fixture,g_src_done,g_src_renaming,g_src_cancel,g_src_err;static long long g_src_avail,g_src_read_cursor;static int g_src_segmented;static FILE*files[8];static unsigned ticks;static MvSourceSnapshot snapshot;
static char*getcwd(char*out,size_t cap){snprintf(out,cap,"%s",root);return out;}
static unsigned sceKernelGetSystemTimeLow(void){return ticks+=1000;}
static int sceKernelDelayThread(int n){(void)n;if(g_src_renaming){g_src_done=1;g_src_renaming=0;}return 0;}
static MvSourceSnapshot src_snapshot(void){return snapshot;}
static int mv_stream_needs_range(long long off,long long available,long long total){(void)off;(void)available;(void)total;return 0;}
static int src_range_read(long long off,void*b,int n,long long total){(void)off;(void)b;(void)n;(void)total;return 0;}
static int sceIoOpen(const char*p,int flags,int mode){(void)flags;(void)mode;if(!strchr(p,':')&&p[0]!='/')return -2147351764;FILE*f=fopen(p,"rb");if(!f)return -1;for(int i=1;i<8;i++)if(!files[i]){files[i]=f;return i;}return -1;}
static long long sceIoLseek(int fd,SceOff off,int whence){return fseek(files[fd],(long)off,whence)?-1:off;}
static int sceIoRead(int fd,void*b,int n){return (int)fread(b,1,n,files[fd]);}
static int sceIoClose(int fd){int result=fclose(files[fd]);files[fd]=NULL;return result;}
'''+fn('static const char *src_path(')+fn('static int src_resolve_path(')+fn('static int src_read_at(')+r'''
int main(int argc,char**argv){assert(argc==2);snprintf(root,sizeof(root),"%s",argv[1]);assert(!src_resolve_path(g_src_final,sizeof(g_src_final),"./final.mp4"));assert(!src_resolve_path(g_src_part,sizeof(g_src_part),"./partial.mp4"));FILE*f=fopen(g_src_part,"wb");assert(f);fputs("0123456789abcdefghijklmnopqrstuvwxyz",f);fclose(f);snapshot=(MvSourceSnapshot){36,36,0,0,0};g_src_avail=36;char out[10]={0};assert(sceIoOpen("./partial.mp4",1,0)<0);assert(src_read_at(NULL,10,out,6)==6&&!memcmp(out,"abcdef",6));assert(!rename(g_src_part,g_src_final));g_src_renaming=1;assert(src_read_at(NULL,20,out,4)==4&&!memcmp(out,"klmn",4));snapshot.done=1;assert(src_read_at(NULL,36,out,5)==0);snapshot.cancel=1;assert(src_read_at(NULL,0,out,5)<0);char shortpath[4];assert(src_resolve_path(shortpath,sizeof(shortpath),"./long-path")<0);puts("PASS production source callback: independent PRX cwd, correct offset bytes, rename publication race, true EOF/cancel and long-path rejection");}
'''
with tempfile.TemporaryDirectory(prefix='mv-callback-')as t:
 p=Path(t);(p/'test.c').write_text(code);exe=p/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-I'+str(R/'src'),p/'test.c','-o',exe],check=True);subprocess.run([exe,p.as_posix()],check=True)
