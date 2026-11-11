"""IME pack regressions: missing words, >512 candidates, corrupt update rollback."""
from pathlib import Path
import subprocess
import sys
R = Path(__file__).resolve().parents[1]
O = R / '.tmp/ime-pack-test'
O.mkdir(parents=True, exist_ok=True)
# First regression uses the existing public API, before adding pack interfaces.
code = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ime.h"
static int has(const char *s) { for(int i=0;i<ime_candidate_count();i++) if(!strcmp(s,ime_candidate(i))) return 1; return 0; }
int main(void) {
 ime_set_mode(IME_PINYIN);
 const char *p="shijian"; while(*p) ime_feed(*p++);
 assert(has("视奸")); assert(has("时间")); assert(has("事件")); assert(has("实践"));
 ime_reset(); ime_feed('s'); assert(ime_candidate_count()>512);
 puts("PASS missing shijian phrases and uncapped candidate coverage");
}
'''
(O/'coverage.c').write_text(code, encoding='utf-8')
cc = ['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O1','-Wall','-Wextra','-Werror','-I'+str(R/'src')]
sources=[str(R/'src/ime.c')]
if (R/'src/ime_pack.c').exists(): sources.append(str(R/'src/ime_pack.c'))
subprocess.run(cc+[str(O/'coverage.c')]+sources+['-o',str(O/'coverage.exe')],check=True)
subprocess.run([str(O/'coverage.exe')],check=True)
if '--coverage-only' in sys.argv: sys.exit(0)
import struct
import zlib
import shutil
import json
import importlib.util
spec = importlib.util.spec_from_file_location('ime_builder', R/'tools/build_ime_dict.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
# Use the actual generated artifact; generator must be deterministic.
source = R/'tools/ime/current.bin'
subprocess.run([sys.executable, str(R/'tools/build_ime_dict.py'), '--pack', str(O/'generated.bin')], check=True)
assert source.read_bytes() == (O/'generated.bin').read_bytes()
original = source.read_bytes()
(O/'valid.bin').write_bytes(original)
updated = bytearray(original)
struct.pack_into('<I', updated, 44, 20261112)
(O/'next.bin').write_bytes(updated)
# One independently malformed file for each boundary; CRC is repaired when
# exercising semantic validation rather than transport corruption.
cases = {}
def case(name, mutate, repair_crc=False):
    data = bytearray(original)
    mutate(data)
    if repair_crc: struct.pack_into('<I', data, 20, zlib.crc32(data[64:]))
    (O/name).write_bytes(data)
    cases[name] = True
case('bad-version.bin', lambda d: struct.pack_into('<I', d, 8, 999))
case('bad-size.bin', lambda d: struct.pack_into('<I', d, 16, 0xffffffff))
case('bad-count.bin', lambda d: struct.pack_into('<I', d, 24, 65536))
case('bad-crc.bin', lambda d: struct.pack_into('<I', d, 20, 0))
case('bad-string.bin', lambda d: d.__setitem__(slice(64,112), b'x'*48), True)
case('bad-utf8.bin', lambda d: d.__setitem__(slice(112,116), b'\xf0\x80\x80\x80'), True)
case('bad-font.bin', lambda d: d.__setitem__(slice(112,136), '😀'.encode()+b'\0'*20), True)
case('bad-sort.bin', lambda d: d.__setitem__(slice(64,112), b'zzzz'+b'\0'*44), True)
case('bad-kind.bin', lambda d: struct.pack_into('<I', d, 140, 2), True)
(O/'truncated.bin').write_bytes(original[:-1])
(O/'trailing.bin').write_bytes(original+b'x')
pack_code = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "ime.h"
static void type(const char *s) { ime_reset();while(*s)ime_feed(*s++); }
static int has(const char *s) { for(int i=0;i<ime_candidate_count();i++)if(!strcmp(s,ime_candidate(i)))return 1;return 0; }
int main(int argc,char **argv) {
 assert(argc==2);char path[1024],valid[1024],next[1024],current[1024],out[24];ImePack *p=NULL;
 snprintf(valid,sizeof(valid),"%s/valid.bin",argv[1]);snprintf(next,sizeof(next),"%s/next.bin",argv[1]);snprintf(current,sizeof(current),"%s/current.bin",argv[1]);
 const char *bad[]={"bad-version.bin","bad-size.bin","bad-count.bin","bad-crc.bin","bad-string.bin","bad-utf8.bin","bad-font.bin","bad-sort.bin","bad-kind.bin","truncated.bin","trailing.bin"};
 assert(!ime_pack_prepare(valid,&p));assert(p);assert(ime_pack_revision(p)==20261111);
 assert(ime_pack_count(p,0)>32000);ImePackEntry e;assert(ime_pack_read(p,0,999999,&e)==IME_PACK_BOUNDS);
 ime_activate_pack(p);ime_set_mode(IME_PINYIN);
 type("shijian");assert(has("视奸")&&has("时间")&&has("事件")&&has("实践"));
 type("chuyin");assert(has("初音")&&has("初音未来"));type("chuyinweilai");assert(has("初音未来"));
 type("chongqing");assert(has("重庆"));type("yinhang");assert(has("银行"));
 type("zhong");assert(has("重"));type("chong");assert(has("重"));
 clock_t start=clock();type("s");int total=ime_candidate_total();assert(total>512);
 char window[4][IME_WORD_MAX];assert(ime_candidate_window(600,window,4)==4);
 for(int i=0;i<4;i++)assert(!strcmp(window[i],ime_candidate(600+i)));
 assert(ime_candidate_window(total-1,window,4)==1);assert(ime_candidate_window(-1,window,4)==0);
 assert(!*ime_candidate(total));assert(!*ime_candidate(-1));
 assert(ime_select(600,out,sizeof(out))>0);assert(!ime_active());
 printf("pack candidates s=%d; query+window+select host CPU %.3fs\n",total,(double)(clock()-start)/CLOCKS_PER_SEC);
 assert(!ime_pack_install(valid,current));
 ImePack *old=NULL;assert(!ime_pack_prepare(current,&old));
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
  snprintf(path,sizeof(path),"%s/%s",argv[1],bad[i]);
  p=(ImePack*)1;assert(ime_pack_prepare(path,&p)<0);assert(!p);
  assert(ime_pack_install(path,current)<0);assert(!ime_pack_prepare(current,&p));assert(ime_pack_revision(p)==20261111);ime_pack_close(p);
 }
 assert(!ime_pack_install(next,current));assert(ime_pack_revision(old)==20261111);assert(!ime_pack_read(old,0,0,&e));
 assert(!ime_pack_prepare(current,&p));assert(ime_pack_revision(p)==20261112);ime_activate_pack(p);ime_pack_close(old);
 assert(ime_dictionary_revision()==20261112);assert(!ime_active());type("chuyin");assert(has("初音"));
 assert(ime_load_dictionary("path-that-does-not-exist")<0);assert(ime_dictionary_revision()==0);type("chuyin");assert(has("初音"));
 ime_activate_pack(NULL);puts("PASS pack validation, immutable open reader, rollback, activation, fallback, bounds and font coverage");
 return 0;
}
'''
pack_test = O/'pack.c'
pack_test.write_text(pack_code,encoding='utf-8')
subprocess.run(cc+[str(pack_test)]+sources+['-o',str(O/'pack.exe')],check=True)
subprocess.run([str(O/'pack.exe'), str(O)],check=True)
# Startup pointer accepts a basename only and never follows traversal/NUL data.
startup = O/'startup'
(startup/'ime').mkdir(parents=True, exist_ok=True)
shutil.copyfile(O/'valid.bin', startup/'ime/current.bin')
shutil.copyfile(O/'next.bin', startup/'ime/pack-new.bin')
startup_code = r'''
#include <assert.h>
#include <stdlib.h>
#include "ime.h"
int main(int argc,char **argv) {
 assert(argc==3);assert(!ime_load_dictionary(argv[1]));
 assert(ime_dictionary_revision()==(unsigned)strtoul(argv[2],0,10));
 ime_activate_pack(0);return 0;
}
'''
(O/'startup.c').write_text(startup_code,encoding='utf-8')
subprocess.run(cc+[str(O/'startup.c')]+sources+['-o',str(O/'startup.exe')],check=True)
for pointer in [b'pack-new.bin', b'pack-new.bin\n', b'pack-new.bin\r\n']:
    (startup/'ime/active.txt').write_bytes(pointer)
    subprocess.run([str(O/'startup.exe'),str(startup),'20261112'],check=True)
for pointer in [b'',b'..',b'.',b'../pack-new.bin',b'foo/pack-new.bin',b'foo\\pack-new.bin',
                b'pack-new.bin\0',b'pack-new.bin\nextra',b'x'*81,b'not-present.bin',
                b'pack-new.bin\r',b'pack-new.bin\n\n']:
    (startup/'ime/active.txt').write_bytes(pointer)
    subprocess.run([str(O/'startup.exe'),str(startup),'20261111'],check=True)
print('PASS active basename pointer, traversal/NUL/length rejection and current fallback')
print('PASS IME pack regression driver')
