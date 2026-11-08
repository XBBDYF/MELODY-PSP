"""Production MV metadata transaction keeps old index on write/rename failure."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];s=(R/'src/mv.c').read_text(encoding='utf-8');a=s.index('static void mv_index_store(');b=s.index('\n}',a)+2
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#define MV_INDEX_BYTES 16384
#define MV_INDEX_PATH "mv_index.txt"
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 512
#define PSP_O_TRUNC 1024
#define mlog(...) ((void)0)
typedef struct{long long st_size;}SceIoStat;static FILE*files[8];static int mode,g_snap_last=123;
static int sceIoOpen(const char*p,int f,int m){(void)m;FILE*fp=fopen(p,f&2?"wb":"rb");if(!fp)return -1;for(int i=1;i<8;i++)if(!files[i]){files[i]=fp;return i;}return -1;}
static int sceIoRead(int fd,void*b,int n){return(int)fread(b,1,n,files[fd]);}
static int sceIoWrite(int fd,const void*b,int n){if(mode==1)return -1;return(int)fwrite(b,1,n,files[fd]);}
static int sceIoClose(int fd){int r=fclose(files[fd]);files[fd]=NULL;return r;}
static int sceIoRemove(const char*p){return remove(p);}
static int sceIoRename(const char*a,const char*b){if(mode==2&&strstr(a,".tmp"))return -1;return rename(a,b);}
static int sceIoGetstat(const char*p,SceIoStat*out){struct stat s;if(stat(p,&s))return -1;out->st_size=s.st_size;return 0;}
'''+s[a:b]+r'''
int main(int argc,char**argv){assert(argc==2);mode=atoi(argv[1]);FILE*f=fopen(MV_INDEX_PATH,"wb");assert(f);fputs("1\t100\told\tartist\n",f);fclose(f);mv_index_store(2,"2\t200\tnew\tartist\n");char text[256]={0};f=fopen(MV_INDEX_PATH,"rb");assert(f);fread(text,1,sizeof(text)-1,f);fclose(f);assert(strstr(text,"1\t100\told\tartist"));if(!mode)assert(strstr(text,"2\t200\tnew\tartist")&&g_snap_last==0);else assert(!strstr(text,"new")&&g_snap_last==123);puts("PASS actual MV index transaction preserves prior complete rows under failure");}
'''
with tempfile.TemporaryDirectory(prefix='mv-index-transaction-')as t:
 p=Path(t);(p/'test.c').write_text(code);exe=p/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',p/'test.c','-o',exe],check=True)
 for mode in ['0','1','2']:subprocess.run([exe,mode],cwd=p,check=True)
