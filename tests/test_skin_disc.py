"""Compile the real disc module; inspect its public drawing and texture output."""
import ctypes
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DiscTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('gcc')
        if not compiler:
            raise unittest.SkipTest('host gcc required')
        cls.tmp = tempfile.TemporaryDirectory()
        build = pathlib.Path(cls.tmp.name)
        (build / 'pspkernel.h').write_text('typedef int SceUID;\n')
        (build / 'pspsysmem.h').write_text('''#define PSP_SMEM_High 1
int sceKernelAllocPartitionMemory(int,const char*,int,unsigned int,void*);
void *sceKernelGetBlockHeadAddr(int);
int sceKernelFreePartitionMemory(int);
''')
        (build / 'shim.c').write_text(r'''
#include <stdlib.h>
#include <string.h>
#include "app.h"
AppState g_app;
static unsigned short art[224*224];
static void *block;
int ready, aw=224, ah=224, fail_alloc, fail_head;
int allocs, frees, requested, partition, kind, circles, rings, blits;
float last_angle;
unsigned char output[128*128*4];
const unsigned short *api_art(int *w,int *h){*w=aw;*h=ah;return ready?art:0;}
void set_song(long long id){g_app.now.id=id;}
void set_art(unsigned short color){int i;for(i=0;i<224*224;i++)art[i]=color;}
void set_pixel(int i,unsigned short color){art[i]=color;}
unsigned short gfx_rgb(int r,int g,int b){return (unsigned short)((r>>3)|((g>>2)<<5)|((b>>3)<<11));}
void gfx_circle(int x,int y,int r,unsigned short c){(void)x;(void)y;(void)r;(void)c;circles++;}
void gfx_ring(int x,int y,int ro,int ri,unsigned short c){(void)x;(void)y;(void)ro;(void)ri;(void)c;rings++;}
void gfx_blit_rgba_rotated(int x,int y,int size,const unsigned char*p,int w,int h,float a){
(void)x;(void)y;(void)size;if(w!=128||h!=128)abort();memcpy(output,p,sizeof(output));last_angle=a;blits++;}
int sceKernelAllocPartitionMemory(int p,const char*n,int t,unsigned int s,void*a){
(void)n;(void)a;requested=s;partition=p;kind=t;if(fail_alloc)return -1;block=malloc(s);if(!block)return -1;allocs++;return 7;}
void *sceKernelGetBlockHeadAddr(int id){(void)id;return fail_head?0:block;}
int sceKernelFreePartitionMemory(int id){(void)id;free(block);block=0;frees++;return 0;}
''')
        dll = build / ('disc.dll' if os.name == 'nt' else 'disc.so')
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c99', '-O2',
                        '-Wall', '-Wextra', '-Werror', '-I', str(build),
                        '-I', str(ROOT / 'src'), str(ROOT / 'src/skin_disc.c'),
                        str(build / 'shim.c'), '-lm', '-o', str(dll)], check=True)
        cls.lib = ctypes.CDLL(str(dll))
        cls.lib.skin_disc_draw.argtypes = [ctypes.c_int] * 3 + [ctypes.c_float]
        cls.lib.skin_disc_draw.restype = ctypes.c_int
        cls.lib.set_song.argtypes = [ctypes.c_longlong]
        cls.output = (ctypes.c_ubyte * (128 * 128 * 4)).in_dll(cls.lib, 'output')

    @classmethod
    def tearDownClass(cls):
        cls.lib.skin_disc_release()
        import _ctypes
        handle = cls.lib._handle
        del cls.lib
        if os.name == 'nt':
            _ctypes.FreeLibrary(handle)
        else:
            _ctypes.dlclose(handle)
        cls.tmp.cleanup()

    def value(self, name, value=None):
        item = ctypes.c_int.in_dll(self.lib, name)
        if value is not None:
            item.value = value
        return item.value

    def setUp(self):
        self.lib.skin_disc_release()
        for name in ('ready', 'fail_alloc', 'fail_head', 'allocs', 'frees',
                     'circles', 'rings', 'blits'):
            self.value(name, 0)
        self.value('aw', 224)
        self.value('ah', 224)
        self.lib.set_song(1)
        self.lib.set_art(0x001f)

    def draw(self, angle=0):
        return self.lib.skin_disc_draw(20, 10, 224, angle)

    def pixel(self, x, y):
        i = (y * 128 + x) * 4
        return list(self.output[i:i + 4])

    def test_placeholder_without_cover_allocates_nothing(self):
        self.assertEqual(self.draw(), 0)
        self.assertGreater(self.value('circles'), 0)
        self.assertEqual(self.value('allocs'), 0)
        self.assertEqual(self.value('blits'), 0)

    def test_real_red_low_565_cover_has_transparent_circle_corners(self):
        self.value('ready', 1)
        self.assertEqual(self.draw(405), 1)
        for x, y in ((0, 0), (127, 0), (0, 127), (127, 127)):
            self.assertEqual(self.pixel(x, y)[3], 0)
        self.assertEqual(self.pixel(64, 64), [255, 0, 0, 255])
        self.assertAlmostEqual(ctypes.c_float.in_dll(self.lib, 'last_angle').value, 45)
        self.assertEqual(self.value('requested'), 65536)
        self.assertEqual(self.value('partition'), 2)
        self.assertEqual(self.value('kind'), 1)

    def test_cache_reuses_allocation_and_refreshes_reused_art_pointer(self):
        self.value('ready', 1)
        self.assertEqual(self.draw(), 1)
        self.assertEqual(self.draw(-90), 1)
        self.assertEqual(self.value('allocs'), 1)
        self.assertAlmostEqual(ctypes.c_float.in_dll(self.lib, 'last_angle').value, 270)
        self.lib.set_art(0xf800)
        self.assertEqual(self.draw(), 1)
        self.assertEqual(self.pixel(64, 64), [0, 0, 255, 255])
        self.lib.set_song(2)
        self.lib.set_art(0x07e0)
        self.assertEqual(self.draw(), 1)
        self.assertEqual(self.pixel(64, 64), [0, 255, 0, 255])
        self.assertEqual(self.value('allocs'), 1)

    def test_rectangular_cover_is_center_cropped(self):
        self.value('ready', 1)
        self.value('ah', 112)
        # Blue strips on a wide red image must be cropped off, not squeezed in.
        for y in range(112):
            for x in list(range(56)) + list(range(168, 224)):
                self.lib.set_pixel(y * 224 + x, 0xf800)
        self.assertEqual(self.draw(), 1)
        self.assertEqual(self.pixel(0, 64), [255, 0, 0, 255])
        self.assertEqual(self.pixel(127, 64), [255, 0, 0, 255])
        self.assertEqual(self.pixel(64, 64), [255, 0, 0, 255])

    def test_song_identity_refreshes_even_when_samples_do_not_change(self):
        self.value('ready', 1)
        self.assertEqual(self.draw(), 1)
        # Texture center is source (112,112), away from all 16 hash samples.
        self.lib.set_pixel(112 * 224 + 112, 0xf800)
        self.lib.set_song(2)
        self.assertEqual(self.draw(), 1)
        self.assertEqual(self.pixel(64, 64), [0, 0, 255, 255])

    def test_nonfinite_angle_becomes_zero(self):
        self.value('ready', 1)
        for angle in (float('nan'), float('inf'), float('-inf')):
            self.assertEqual(self.draw(angle), 1)
            self.assertEqual(ctypes.c_float.in_dll(self.lib, 'last_angle').value, 0)

    def test_not_ready_then_ready_same_song(self):
        self.value('ready', 1)
        self.value('aw', 0)
        self.assertEqual(self.draw(), 0)
        self.value('aw', 224)
        self.assertEqual(self.draw(), 1)
        self.value('ready', 0)
        self.assertEqual(self.draw(), 0)
        self.assertEqual(self.value('blits'), 1)

    def test_allocation_and_block_head_failure_keep_vinyl(self):
        self.value('ready', 1)
        self.value('fail_alloc', 1)
        self.assertEqual(self.draw(), 0)
        self.assertGreater(self.value('circles'), 0)
        self.value('fail_alloc', 0)
        self.value('fail_head', 1)
        self.assertEqual(self.draw(), 0)
        self.assertEqual(self.value('frees'), 1)
        self.value('fail_head', 0)
        self.assertEqual(self.draw(), 1)

    def test_release_is_idempotent_and_reacquires_on_demand(self):
        self.value('ready', 1)
        self.assertEqual(self.draw(), 1)
        self.lib.skin_disc_release()
        self.lib.skin_disc_release()
        self.assertEqual(self.value('frees'), 1)
        self.assertEqual(self.draw(), 1)
        self.assertEqual(self.value('allocs'), 2)


if __name__ == '__main__':
    unittest.main()
