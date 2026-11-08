"""Exercise the production page primitive against guarded host backbuffers."""
import ctypes
import math
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
W, H, STRIDE = 480, 272, 512
NEW = 0xF81F


class PageTurnTests(unittest.TestCase):
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
        shim = build / 'shim.c'
        shim.write_text('''#include <stdlib.h>
#include <string.h>
int gfx_renderer(void){return 0;}
int gfx_ge_page_quad(const unsigned short*p,int sw,int sh,int stride,const float xy[8],const float uv[8],unsigned short tint){(void)p;(void)sw;(void)sh;(void)stride;(void)xy;(void)uv;(void)tint;return 0;}
static unsigned short storage[512*272+32];
static void *block;
int allocs, frees, fail_alloc, fail_head, null_fb, requested, partition, kind;
int clip[4]={0,0,480,272};
unsigned short *gfx_backbuffer(void){return null_fb?0:storage+16;}
unsigned short *test_storage(void){return storage;}
void gfx_get_clip(int*a,int*b,int*c,int*d){*a=clip[0];*b=clip[1];*c=clip[2];*d=clip[3];}
int sceKernelAllocPartitionMemory(int p,const char*n,int t,unsigned int s,void*a){
(void)n;(void)a;requested=s;partition=p;kind=t;if(fail_alloc)return -1;
block=malloc(s);if(!block)return -1;allocs++;return 9;}
void *sceKernelGetBlockHeadAddr(int id){(void)id;return fail_head?0:block;}
int sceKernelFreePartitionMemory(int id){(void)id;free(block);block=0;frees++;return 0;}
''')
        dll = build / ('page.dll' if os.name == 'nt' else 'page.so')
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c99', '-O2',
                        '-Wall', '-Wextra', '-Werror', '-I', str(build),
                        '-I', str(ROOT / 'src'), str(ROOT / 'src/skin_page_turn.c'),
                        str(shim), '-lm', '-o', str(dll)], check=True)
        cls.lib = ctypes.CDLL(str(dll))
        cls.lib.skin_page_turn.argtypes = [ctypes.c_float, ctypes.c_int,
                                           ctypes.c_int, ctypes.c_ushort]
        cls.lib.skin_page_fold.argtypes = [ctypes.c_float] + [ctypes.c_int] * 4
        cls.lib.test_storage.restype = ctypes.POINTER(ctypes.c_ushort)
        cls.storage = cls.lib.test_storage()
        cls.fb = ctypes.cast(ctypes.addressof(cls.storage.contents) + 32,
                             ctypes.POINTER(ctypes.c_ushort))
        cls.clip = (ctypes.c_int * 4).in_dll(cls.lib, 'clip')

    @classmethod
    def tearDownClass(cls):
        cls.lib.skin_page_release()
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
        self.lib.skin_page_release()
        for name in ('allocs', 'frees', 'fail_alloc', 'fail_head', 'null_fb'):
            self.value(name, 0)
        self.clip[:] = (0, 0, W, H)
        for i in range(STRIDE * H + 32):
            self.storage[i] = 0xA55A
        self.old = [((x * 13 + y * 29) & 0xFFFF)
                    for y in range(H) for x in range(W)]
        for y in range(H):
            for x in range(W):
                self.fb[y * STRIDE + x] = self.old[y * W + x]

    def capture(self):
        self.assertEqual(self.lib.skin_page_capture(), 1)
        self.assertEqual(self.lib.skin_page_active(), 1)
        for y in range(H):
            for x in range(W):
                self.fb[y * STRIDE + x] = NEW

    def check_guards(self):
        self.assertTrue(all(self.storage[i] == 0xA55A for i in range(16)))
        self.assertTrue(all(self.storage[16 + STRIDE * H + i] == 0xA55A
                            for i in range(16)))
        self.assertTrue(all(self.fb[y * STRIDE + x] == 0xA55A
                            for y in range(H) for x in range(W, STRIDE)))

    def test_closed_exact_pattern_and_clip(self):
        self.capture()
        self.clip[:] = (17, 21, 453, 249)
        self.lib.skin_page_turn(0, 30, 230, 0)
        for y in range(H):
            for x in range(W):
                expected = self.old[y * W + x] if 17 <= x < 453 and 30 <= y < 230 else NEW
                self.assertEqual(self.fb[y * STRIDE + x], expected)
        self.check_guards()
        self.assertEqual(self.value('requested'), W * H * 2)
        self.assertLessEqual(self.value('requested') + 128, 262144 + 128)
        self.assertEqual((self.value('partition'), self.value('kind')), (2, 1))

    def test_open_does_not_write_or_release(self):
        self.capture()
        for progress in (1, 2, float('inf')):
            self.lib.skin_page_turn(progress, 0, H, 0)
        self.assertTrue(all(self.fb[y * STRIDE + x] == NEW
                            for y in range(H) for x in range(W)))
        self.assertEqual(self.lib.skin_page_active(), 1)
        self.check_guards()

    def test_half_fold_left_spine_inverse_projection_preserves_underlay(self):
        self.capture()
        self.clip[:] = (31, 12, 467, 245)
        rx, ry, rw, rh = 24, 26, 320, 226
        self.lib.skin_page_fold(.5, rx, ry, rw, rh)
        sine = math.sin(math.pi / 4)
        cosine = math.cos(math.pi / 4)
        projected_left = rx * cosine * 420 / (420 + rx * sine)
        projected_right = (rx + rw) * cosine * 420 / (420 + (rx + rw) * sine)
        center = ry + rh * .5
        light = 256 - int(80 * sine)
        included = revealed = 0
        for x in range(W):
            dx = x
            valid_column = projected_left <= dx < projected_right
            sx = dx * 420 / (420 * cosine - dx * sine) if valid_column else 0
            scale = 420 / (420 + sx * sine)
            upper = center - rh * .5 * scale
            lower = center + rh * .5 * scale
            for y in range(H):
                actual = self.fb[y * STRIDE + x]
                inside = valid_column and 31 <= x < 467 and 12 <= y < 245 and math.ceil(upper) <= y < math.ceil(lower)
                if valid_column and 31 <= x < 467 and 12 <= y < 245 and min(abs(y - upper), abs(y - lower)) < .0001:
                    if actual == NEW:
                        continue
                    inside = True
                if not inside:
                    self.assertEqual(actual, NEW, (x, y))
                    revealed += rx <= x < rx + rw and ry <= y < ry + rh
                    continue
                included += 1
                if y - upper < 1.0001 or lower - y <= 1.0001 or projected_right - dx <= 1.0001 or dx - projected_left < 1.0001:
                    self.assertNotEqual(actual, NEW)
                    continue
                sy = (y - center) / scale + rh * .5
                candidates = set()
                for yy in range(max(0, int(sy) - 1), min(rh, int(sy) + 2)):
                    for xx in range(max(rx, int(sx) - 1), min(rx + rw, int(sx) + 2)):
                        c = self.old[(ry + yy) * W + xx]
                        candidates.add(((c & 31) * light >> 8) |
                                       ((((c >> 5) & 63) * light >> 8) << 5) |
                                       ((((c >> 11) & 31) * light >> 8) << 11))
                self.assertIn(actual, candidates, (x, y))
        self.assertGreater(included, 1000)
        self.assertGreater(revealed, 1000)
        self.assertEqual(self.value('allocs'), 1)
        self.check_guards()

    def test_independent_planes_closed_open_and_stagger(self):
        self.capture()
        self.lib.skin_page_fold(0, 10, 20, 90, 80)
        self.lib.skin_page_fold(1, 120, 20, 90, 80)
        self.lib.skin_page_fold(.5, 230, 20, 90, 80)
        for y in range(H):
            for x in range(W):
                actual = self.fb[y * STRIDE + x]
                if 10 <= x < 100 and 20 <= y < 100:
                    self.assertEqual(actual, self.old[y * W + x])
                elif not (117 <= x < 148 and 20 <= y < 100):
                    self.assertEqual(actual, NEW)
        self.assertNotEqual(self.fb[60 * STRIDE + 120], NEW)
        self.assertEqual(self.fb[60 * STRIDE + 319], NEW)
        self.assertEqual(self.value('allocs'), 1)
        self.check_guards()

    def test_offset_plane_uses_global_zero_spine(self):
        self.capture()
        self.lib.skin_page_fold(.5, 200, 40, 80, 120)
        sine = math.sin(math.pi / 4)
        left = math.ceil(200 * sine * 420 / (420 + 200 * sine))
        right = math.ceil(280 * sine * 420 / (420 + 280 * sine))
        self.assertLess(right, 200)
        for x in range(W):
            if left <= x < right:
                self.assertNotEqual(self.fb[100 * STRIDE + x], NEW)
            else:
                self.assertEqual(self.fb[100 * STRIDE + x], NEW)
        self.assertEqual(self.value('allocs'), 1)
        self.check_guards()

    def test_recaptured_new_plane_unfolds_without_second_allocation(self):
        self.capture()
        # New page has now been rendered; overwrite the same capture slot.
        self.assertEqual(self.lib.skin_page_capture(), 1)
        previous_count = -1
        for progress in (1, .75, .5, .25, 0):
            for y in range(H):
                for x in range(W):
                    self.fb[y * STRIDE + x] = 0
            self.lib.skin_page_fold(progress, 100, 50, 180, 140)
            count = 0
            for y in range(H):
                for x in range(W):
                    actual = self.fb[y * STRIDE + x]
                    if not (0 <= x < 280 and 50 <= y < 190):
                        self.assertEqual(actual, 0)
                    if actual:
                        count += 1
                    if progress == 0 and 100 <= x < 280 and 50 <= y < 190:
                        self.assertEqual(actual, NEW)
            self.assertGreaterEqual(count, previous_count)
            previous_count = count
        self.assertEqual(previous_count, 180 * 140)
        self.assertEqual(self.value('allocs'), 1)
        self.check_guards()

    def test_fold_clipped_rectangle_and_extreme_bounds(self):
        self.capture()
        self.clip[:] = (3, 4, 31, 32)
        self.lib.skin_page_fold(0, -10, -20, 40, 50)
        for y in range(H):
            for x in range(W):
                expected = self.old[y * W + x] if 3 <= x < 30 and 4 <= y < 30 else NEW
                self.assertEqual(self.fb[y * STRIDE + x], expected)
        before = tuple(self.storage[i] for i in range(STRIDE * H + 32))
        for rect in ((0, 0, 0, 1), (0, 0, 1, -1), (2147483647, 0, 2147483647, 1), (-2147483648, 0, 1, 1)):
            self.lib.skin_page_fold(.5, *rect)
        self.assertEqual(tuple(self.storage[i] for i in range(STRIDE * H + 32)), before)
        self.check_guards()

    def test_reuse_release_and_failure(self):
        self.capture()
        self.assertEqual(self.lib.skin_page_capture(), 1)
        self.assertEqual(self.value('allocs'), 1)
        self.value('null_fb', 1)
        self.assertEqual(self.lib.skin_page_capture(), 0)
        self.assertEqual(self.lib.skin_page_active(), 1)
        self.lib.skin_page_turn(.5, 0, H, 0)
        self.value('null_fb', 0)
        self.lib.skin_page_release()
        self.lib.skin_page_release()
        self.assertEqual(self.value('frees'), 1)
        self.assertEqual(self.lib.skin_page_active(), 0)
        self.lib.skin_page_turn(.5, 0, H, 0)
        self.value('fail_alloc', 1)
        self.assertEqual(self.lib.skin_page_capture(), 0)
        self.value('fail_alloc', 0)
        self.value('fail_head', 1)
        self.assertEqual(self.lib.skin_page_capture(), 0)
        self.assertEqual(self.lib.skin_page_active(), 0)
        self.assertEqual(self.value('frees'), 2)
        self.check_guards()

    def test_invalid_bounds_progress_and_clip(self):
        self.capture()
        for p in (-1, float('-inf'), float('nan')):
            self.lib.skin_page_turn(p, -999, 999, 0)
            self.assertTrue(all(self.fb[y * STRIDE + x] == self.old[y * W + x]
                                for y in range(H) for x in range(W)))
        before = tuple(self.storage[i] for i in range(STRIDE * H + 32))
        for top, bottom in ((200, 20), (12, 12), (999, 1000), (-2, -1)):
            self.lib.skin_page_turn(.5, top, bottom, 0)
        self.clip[:] = (999, 999, -999, -999)
        self.lib.skin_page_turn(.5, 0, H, 0)
        self.assertEqual(tuple(self.storage[i] for i in range(STRIDE * H + 32)), before)
        self.clip[:] = (-999, -999, 999, 999)
        self.lib.skin_page_turn(.5, -999, 999, 0)
        self.check_guards()


if __name__ == '__main__':
    unittest.main()
