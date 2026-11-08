"""Exercise the production gate against a coalescing partition allocation fake."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'src/mv.c').read_text(encoding='utf-8')
match = re.search(r'static int mv_memory_gate_blocks\([^\n]+\)\s*\{.*?\n\}', source, re.S)
assert match, 'MV needs one testable gate policy; do not relax the contiguous requirement'
out = root / '.tmp/mv-fragmentation-test'
out.mkdir(parents=True, exist_ok=True)
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* KiB units: a transient 4100KiB Low block, a pinned 96KiB worker
 * stack, then the 9832KiB remaining hole. Freeing the temporary block
 * gives the observed 13932 total but does not coalesce past the stack. */
static unsigned char used[14028];
static int alloc_low(int n) {
    int run=0;
    for(int i=0;i<(int)sizeof(used);i++) {
        run=used[i]?0:run+1;
        if(run==n) { memset(used+i-n+1,1,n); return i-n+1; }
    }
    return -1;
}
static void release(int p,int n) { assert(p>=0); memset(used+p,0,n); }
static int total(void) { int n=0; for(int i=0;i<(int)sizeof(used);i++) n+=!used[i]; return n; }
static int largest(void) { int n=0,run=0; for(int i=0;i<(int)sizeof(used);i++) {run=used[i]?0:run+1;if(run>n)n=run;}return n; }
'''
tail = r'''
int main(void) {
    int transient=alloc_low(4100), pinned_stack=alloc_low(96);
    release(transient,4100);
    assert(total()==13932 && largest()==9832);
    assert(mv_memory_gate_blocks(total(),largest(),13664,0));
    /* Merely checking max >= image size permits load, then heap fails. */
    int image=alloc_low(6212); assert(image>=0);
    assert(total()==7720 && largest()==4100);
    assert(alloc_low(7168)<0);
    release(image,6212); release(pinned_stack,96);
    assert(largest()==14028);
    assert(!mv_memory_gate_blocks(total(),largest(),13664,0));
    assert(!mv_memory_gate_blocks(13932,9832,13664,1));
    assert(!mv_memory_gate_blocks(1,1,0,0));
    assert(mv_memory_gate_blocks(14000,7000,7456,0));
    puts("PASS: split 13932/9832 refuses; image-first leaves 4100 gap and fails 7MiB; releasing pin coalesces; overrides preserved");
}
'''
(out / 'test.c').write_text(prefix + match.group() + tail, encoding='utf-8')
exe = out / 'test.exe'
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c11', '-Wall', '-Wextra', '-Werror', str(out / 'test.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
# Diagnostic boundaries distinguish releases without exposing paths/content.
for tag in ('preview-freed', 'skin-freed', 'buffers-freed'):
    assert f'mv_step("{tag}")' in source
assert 'sceKernelQueryMemoryPartitionInfo' not in source
assert 'sceKernelGetThreadmanIdList' in source
assert 'sceKernelGetBlockHeadAddr(__psp_heap_blockid)' in source
print('PASS: safe user-mode allocation metadata and per-owner release boundaries')
