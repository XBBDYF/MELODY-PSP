"""Run actual user-mode MV metadata diagnostics against bounded SDK fakes."""
from pathlib import Path
import re
import subprocess
root = Path(__file__).resolve().parents[1]
s = (root / 'src/mv.c').read_text(encoding='utf-8')
fn = re.search(r'static void mv_memory_diag\(void\)\s*\{.*?\n\}', s, re.S)
assert fn
prefix = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef int SceUID;
typedef struct { unsigned size; char name[32]; void *stack; int stackSize; unsigned attr; } SceKernelThreadInfo;
enum { SCE_KERNEL_TMID_Thread=1 };
static int __psp_heap_blockid=19, fail_list, referred, logs;
/* Keep the PSP address cast 32-bit without host pointer-size warnings. */
static unsigned sceKernelGetBlockHeadAddr(int id) { assert(id==19);return 0x08900000; }
static void mlog(const char *format,...) { assert(format); ++logs; }
static int sceKernelGetThreadmanIdList(int type,SceUID *ids,int capacity,int *count) {
    assert(type==SCE_KERNEL_TMID_Thread); assert(capacity==64);
    if(fail_list)return -1;
    for(int i=0;i<capacity;++i)ids[i]=i+1;
    *count=80; return 0;
}
static int sceKernelReferThreadStatus(int id,SceKernelThreadInfo *info) {
    assert(id>=1 && id<=64);assert(info->size==sizeof(*info));++referred;
    if(id==2)return -1;
    memcpy(info->name,"melody_api",11); info->stackSize=98304;
    return 0;
}
'''
# Address values are never dereferenced. Use integer fields on this host fake.
prefix = prefix.replace('void *stack;', 'unsigned stack;')
tail = r'''
int main(void) {
    mv_memory_diag(); assert(referred==64);assert(logs==65);
    fail_list=1; referred=0;logs=0;
    mv_memory_diag();assert(referred==0);assert(logs==2);
    puts("PASS: user-mode metadata only; 64 UID capacity and returned-count clamp; denied list/status tolerated");
}
'''
out = root / '.tmp/mv-memory-diag-test'
out.mkdir(parents=True, exist_ok=True)
(out / 'test.c').write_text(prefix+fn.group()+tail,encoding='utf-8')
exe = out / 'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror',str(out/'test.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
