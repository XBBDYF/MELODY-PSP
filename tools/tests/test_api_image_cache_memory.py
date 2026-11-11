"""Host-run the production API image cache allocation, retirement and null guards."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'src/api.c').read_text(encoding='utf-8')

def section(start, end):
    return SOURCE[SOURCE.index(start):SOURCE.index(end, SOURCE.index(start))]

def function(name):
    start = SOURCE.index(name)
    brace = SOURCE.index('{', start)
    depth = 1
    pos = brace + 1
    while depth:
        depth += (SOURCE[pos] == '{') - (SOURCE[pos] == '}')
        pos += 1
    return SOURCE[start:pos]

CODE = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int SceUID;
#define PSP_SMEM_High 1
static int g_thid=-1, allocated, freed, fail_alloc, fail_head, lock_calls;
static size_t requested;
static void *block;
static int sceKernelAllocPartitionMemory(int p,const char*n,int mode,size_t size,void*a){
 assert(p==2 && mode==PSP_SMEM_High); (void)n;(void)a;
 if(fail_alloc)return -1;
 requested=size;block=malloc(size);assert(block);memset(block,0xCD,size);allocated++;return 42;
}
static void *sceKernelGetBlockHeadAddr(int id){assert(id==42);return fail_head?NULL:block;}
static int sceKernelFreePartitionMemory(int id){assert(id==42);free(block);block=NULL;freed++;return 0;}
static int lock_take(int id){(void)id;lock_calls++;return 1;}
static void lock_give(int id){(void)id;}
static unsigned sceKernelGetSystemTimeLow(void){return 0;}
static int g_lock;
static void safe_copy(char*d,size_t n,const char*s){snprintf(d,n,"%s",s);}
'''
CODE += section('#define THUMB_MAX 24', '/* 封面磁盘缓存')
CODE += section('#define AV_MAX 24', 'int api_avatar_slot(')
for signature in ('int api_thumb_slot(', 'const unsigned short *api_thumb_px(',
                  'int api_avatar_slot(', 'const unsigned short *api_avatar_px(',
                  'static int image_request_current('):
    CODE += function(signature) + '\n'
CODE += r'''
static void absent(void){
 assert(api_thumb_slot("x")==-1 && api_avatar_slot("x")==-1);
 assert(!api_thumb_px(0) && !api_avatar_px(0));
 ImageRequest r={"x",0,1};
 assert(!image_request_current(&r,0) && !image_request_current(&r,1));
}
int main(void){
 absent();assert(!lock_calls);
 fail_alloc=1;image_cache_alloc();absent();assert(g_image_cache_block==-1);
 fail_alloc=0;fail_head=1;image_cache_alloc();absent();assert(allocated==1 && freed==1);
 fail_head=0;
 for(int cycle=0;cycle<2;cycle++){
  image_cache_alloc();assert(g_thumbs && g_avs && requested==740352);
  assert(!g_thumbs[0].state && !g_avs[0].state);
  int count=allocated;image_cache_alloc();assert(allocated==count);
  assert(api_thumb_slot("x")==-1 && g_thumb_qn==1);
  assert(api_avatar_slot("y")==-1 && g_av_qn==1);
  ImageRequest r=g_thumb_q[0];assert(image_request_current(&r,0));
  g_thumbs[0].state=2;g_thumbs[0].px[0]=123;
  g_avs[0].state=2;g_avs[0].px[0]=456;
  assert(*api_thumb_px(0)==123 && *api_avatar_px(0)==456);
  g_thid=7;count=freed;image_cache_release();assert(freed==count && g_thumbs);
  g_thid=-1;image_cache_release();assert(freed==count+1);
  assert(!g_thumbs && !g_avs && !g_thumb_qn && !g_av_qn);
  assert(!image_request_current(&r,0));
  image_cache_release();assert(freed==count+1);absent();
 }
 puts("PASS production image cache: 723KiB partition, allocation failures, active-worker guard, two retire/restart cycles, null accessors");
 return 0;
}
'''

def test_api_image_cache_memory():
    init = function('void api_init(')
    release = function('void api_free_buffers(')
    assert init.index('image_cache_alloc();') < init.index('home_thumbs_restore();') < init.index('sceKernelCreateThread(')
    assert 'image_cache_release();' in release
    with tempfile.TemporaryDirectory(prefix='api_images_') as directory:
        build = Path(directory)
        source = build / 'test.c'
        source.write_text(CODE, encoding='utf-8')
        exe = build / 'test.exe'
        subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c99', '-Wall', '-Wextra', str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    test_api_image_cache_memory()
