"""Execute production bundle installer against host filesystem and actual release PBP."""
from pathlib import Path
import subprocess,tempfile,json,shutil,hashlib,struct,zlib,sys
R=Path(__file__).resolve().parents[1];O=R/'.tmp/update34-tests';O.mkdir(exist_ok=True)
code=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
typedef long long SceOff;typedef struct{long long st_size;}SceIoStat;
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 512
#define PSP_O_TRUNC 1024
#define PSP_SEEK_SET SEEK_SET
#define PSP_SEEK_END SEEK_END
static int writes,renames,fail_write,fail_rename;
static const char*local(const char*p){const char*prefix="./";if(strncmp(p,prefix,strlen(prefix)))abort();return p+strlen(prefix);}
static int sceIoOpen(const char*p,int mode,int perm){int flags=mode&PSP_O_WRONLY?O_WRONLY|O_CREAT|O_TRUNC:O_RDONLY;return open(local(p),flags|O_BINARY,perm);}
static int sceIoRead(int fd,void*p,unsigned n){return (int)read(fd,p,n);}
static int sceIoWrite(int fd,const void*p,unsigned n){if(fail_write&&++writes==fail_write)return -1;return (int)write(fd,p,n);}
static int sceIoClose(int fd){return close(fd);}
static SceOff sceIoLseek(int fd,SceOff at,int mode){return lseek(fd,at,mode);}
static int sceIoSync(const char*p,int mode){(void)p;(void)mode;return 0;}
static int sceIoGetstat(const char*p,SceIoStat*out){struct stat s;int r=stat(local(p),&s);if(!r)out->st_size=s.st_size;return r;}
static int sceIoRemove(const char*p){return remove(local(p));}
static int sceIoRename(const char*a,const char*b){if(strstr(a,".upd34")&&fail_rename&&++renames==fail_rename)return -1;return rename(local(a),local(b));}
static unsigned progress_calls,progress_last;
static void progress(const char*stage,const char*file,unsigned pct){(void)file;if(!strcmp(stage,"Recovering previous update"))progress_last=0;if(pct>100||pct<progress_last)abort();progress_last=pct;progress_calls++;}
#define MB_PROGRESS(stage,name,percent) progress(stage,name,percent)
#include "bundle_update.h"
int main(int argc,char**argv){if(argc>1)fail_write=atoi(argv[1]);if(argc>2)fail_rename=atoi(argv[2]);int result=mb_install();printf("installer=%d progress_calls=%u progress_last=%u\n",result,progress_calls,progress_last);return result<0?2:0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'installer.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(R/'tools/prxloader'),O/'test.c','-lz','-o',exe],check=True)
D=Path(sys.argv[1]).resolve()if len(sys.argv)>1 else R/'releases/MELODY-3.4-update-test';m=json.loads((D/'manifest.json').read_text(encoding='utf-8'));names=list(m['modules']);pbp=((D/'在线更新/EBOOT.PBP')if(D/'在线更新/EBOOT.PBP').exists()else D/'EBOOT.PBP').read_bytes()
def run(p,*args,okay=True):
 v=subprocess.run([exe,*map(str,args)],cwd=p,capture_output=True,text=True);assert (v.returncode==0)==okay,(v.stdout,v.stderr);return v.stdout

def setup(p):
 (p/'EBOOT.PBP').write_bytes(pbp)
 for n in names:(p/n).write_bytes(('old-'+n).encode())
def old(p):
 for n in names:assert (p/n).read_bytes()==('old-'+n).encode(),n
def new(p):
 for n in names:assert hashlib.sha256((p/n).read_bytes()).hexdigest()==m['modules'][n]['sha256'],n
with tempfile.TemporaryDirectory(prefix='melody-update34-')as t:
 p=Path(t);setup(p);first=run(p);assert 'installer=1'in first and 'progress_last=100'in first;new(p);second=run(p);assert 'installer=0'in second and 'progress_calls=0'in second;new(p)
 print('PASS actual8 modules installed, hashes exact, repeat startup skips')
with tempfile.TemporaryDirectory(prefix='melody-update34-')as t:
 p=Path(t);setup(p);bad=bytearray(pbp);bad[-1]^=1;(p/'EBOOT.PBP').write_bytes(bad);run(p,okay=False);old(p);assert not(p/'update34-pending.bin').exists();print('PASS damaged payload never replaces live modules')
with tempfile.TemporaryDirectory(prefix='melody-update34-')as t:
 p=Path(t);setup(p);run(p,2,okay=False);old(p);print('PASS staging short write leaves installed program untouched')
with tempfile.TemporaryDirectory(prefix='melody-update34-')as t:
 p=Path(t);setup(p);run(p,0,4,okay=False);old(p);run(p);new(p);print('PASS mid-commit rename failure rolls back all modules, next attempt succeeds')
with tempfile.TemporaryDirectory(prefix='melody-update34-')as t:
 p=Path(t);setup(p);crc=int(m['bundle_crc32'],16)
 for n in names[:3]:(p/n).rename(p/(n+'.bak34'));(p/n).write_bytes(b'partial-new')
 (p/'update34-pending.bin').write_bytes(struct.pack('<3I',0x4d424a31,crc,255))
 # Recovery happens before CRC validation: corrupted payload tests the old snapshot.
 bad=bytearray(pbp);bad[-1]^=1;(p/'EBOOT.PBP').write_bytes(bad);run(p,okay=False);old(p);print('PASS startup after interrupted commit restores previous complete modules')
for mutation in ['bad_zlib','too_small_output']:
 with tempfile.TemporaryDirectory(prefix='melody-update34-')as t:
  p=Path(t);setup(p);bad=bytearray(pbp);base=struct.unpack_from('<I',bad,36)[0]
  assert struct.unpack_from('<I',bad,base+8)[0]==2
  if mutation=='bad_zlib':
   entry_offset=struct.unpack_from('<I',bad,base+32+64)[0];bad[base+entry_offset]^=0xff
  else:struct.pack_into('<I',bad,base+32+56,1)
  struct.pack_into('<I',bad,base+20,zlib.crc32(bad[base+32:]))
  (p/'EBOOT.PBP').write_bytes(bad);run(p,okay=False);old(p)
  print('PASS compressed bundle rejects',mutation,'before replacing modules')
print('PASS production3.4 bundle installer validation/installation/idempotence/failure recovery')
