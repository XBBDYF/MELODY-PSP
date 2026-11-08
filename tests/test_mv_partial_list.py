"""Production MV index lists incomplete files as retryable, never completed."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];s=(R/'src/mv.c').read_text(encoding='utf-8')
def fn(sig):
 a=s.index(sig);b=s.index('\n}',a)+2;return s[a:b]+'\n'
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "mv.h"
#define MV_INDEX_BYTES 16384
#define MV_CACHE "cache"
#define MV_INDEX_PATH "cache/mv_index.txt"
#define PSP_O_RDONLY 1
typedef struct{long long st_size;}SceIoStat;static FILE*files[8];
static int sceIoOpen(const char*p,int f,int m){(void)f;(void)m;FILE*fp=fopen(p,"rb");if(!fp)return -1;for(int i=1;i<8;i++)if(!files[i]){files[i]=fp;return i;}return -1;}
static int sceIoRead(int fd,void*b,int n){return (int)fread(b,1,n,files[fd]);}
static int sceIoClose(int fd){int r=fclose(files[fd]);files[fd]=NULL;return r;}
static int sceIoGetstat(const char*p,SceIoStat*s){struct stat st;if(stat(p,&st))return -1;s->st_size=st.st_size;return 0;}
static int active;typedef struct{int id;}Info;static Info info={3};
static int api_mv_download_state(void){return active;}
static const Info*api_mv_info(void){return &info;}
'''+fn('static int mv_index_parse(')+fn('static int mv_index_load(')+r'''
int main(void){MvDlItem rows[8];int count=mv_index_load(rows,8);assert(count==3);assert(rows[0].mvid==3&&rows[0].state==3&&rows[0].pct<100);assert(rows[1].mvid==2&&rows[1].state==3&&rows[1].pct==99);assert(strstr(rows[1].path,".part"));assert(rows[2].mvid==1&&rows[2].state==2&&rows[2].pct==100);active=1;count=mv_index_load(rows,8);assert(count==2&&rows[0].mvid==2&&rows[1].mvid==1);puts("PASS actual MV index: near-complete partial retained at99%, retry state/path, completed final100%, active duplicate suppressed");}
'''
with tempfile.TemporaryDirectory(prefix='mv-list-')as t:
 p=Path(t);(p/'cache').mkdir();(p/'cache/mv_index.txt').write_text('1\t100\tcomplete\tartist\n2\t9399957\tpartial\tartist\n3\t1000\tactive\tartist\n4\t1000\tmissing\tartist\n')
 for name,size in [('mv_1_240.mp4',100),('mv_2_240.mp4.part',9387940),('mv_3_240.mp4.part',500)]:
  with(p/'cache'/name).open('wb')as f:f.seek(size-1);f.write(b'X')
 (p/'test.c').write_text(code);exe=p/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-I'+str(R/'src'),p/'test.c','-o',exe],check=True);subprocess.run([exe],cwd=p,check=True)
main=(R/'src/main.c').read_text(encoding='utf-8');assert 'items[g_app.dl_sel].state==3'in main and 'api_request_mv_save(items[g_app.dl_sel].mvid)'in main
print('PASS retry dispatch uses original MV identifier')
