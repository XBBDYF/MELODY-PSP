"""Compile the production asset/JSON/PNG modules with host drawing and memory shims."""
import ctypes
import json
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def png(width=2, height=2):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = (b'\0' + bytes([255, 20, 40, 128]) * width) * height
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


class SkinAssetsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        build = pathlib.Path(cls.tmp.name)
        shim = build / 'shim.c'
        shim.write_text('''#include <stdlib.h>
#include <stdint.h>
unsigned sceKernelGetSystemTimeLow(void){static unsigned t;return t+=5;}
static void *block;
int allocs, frees, draws, last_alpha, source_alpha;
int sceKernelAllocPartitionMemory(int p,const char*n,int t,unsigned int s,void*a) { (void)p;(void)n;(void)t;(void)a; block=malloc(s); if(!block)return -1;allocs++;return 1; }
void *sceKernelGetBlockHeadAddr(int id) { (void)id;return block; }
int sceKernelFreePartitionMemory(int id) { (void)id;free(block);block=0;frees++;return 0; }
void gfx_blit_rgba_scaled(int x,int y,int w,int h,const unsigned char*p,int sw,int sh,int a) { (void)x;(void)y;(void)w;(void)h;(void)sw;(void)sh;draws++;last_alpha=a;source_alpha=p[3]; }
''')
        (build/'pspiofilemgr.h').write_text('#define PSP_O_RDONLY 0\n#define PSP_SEEK_END 2\n#define PSP_SEEK_SET 0\nint sceIoOpen(const char*,int,int);long long sceIoLseek(int,long long,int);int sceIoRead(int,void*,int);int sceIoClose(int);\n')
        with shim.open('a') as f:f.write("\n#include <io.h>\n#include <fcntl.h>\nint sceIoOpen(const char*p,int f,int m){(void)f;(void)m;return _open(p,_O_RDONLY|_O_BINARY);}\nlong long sceIoLseek(int f,long long o,int w){return _lseeki64(f,o,w);}\nint sceIoRead(int f,void*p,int n){return _read(f,p,n);}\nint sceIoClose(int f){return _close(f);}\n")
        (build / 'pspkernel.h').write_text('typedef int SceUID;\nunsigned sceKernelGetSystemTimeLow(void);\n#define PSP_SMEM_High 1\nint sceKernelAllocPartitionMemory(int,const char*,int,unsigned int,void*);\nvoid *sceKernelGetBlockHeadAddr(int);\nint sceKernelFreePartitionMemory(int);\n')
        dll = build / 'assets.dll'
        compiler = shutil.which('gcc')
        if not compiler:
            raise unittest.SkipTest('host gcc required')
        subprocess.run([compiler, '-shared', '-std=c99', '-Wall', '-Wextra', '-I', str(build), '-I', str(ROOT / 'src'), str(ROOT / 'src/skin_assets.c'), str(ROOT / 'src/json.c'), str(ROOT / 'src/png.c'), str(shim), '-lz', '-o', str(dll)], check=True)
        cls.lib = ctypes.CDLL(str(dll))
        cls.lib.skin_assets_validate_manifest.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        cls.lib.skin_assets_probe.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        cls.lib.skin_assets_set_root.argtypes = [ctypes.c_char_p]
        cls.lib.skin_assets_image.argtypes = [ctypes.c_char_p] + [ctypes.c_int] * 5

    @classmethod
    def tearDownClass(cls):
        cls.lib.skin_assets_release()
        import _ctypes,os
        handle=cls.lib._handle;del cls.lib
        if os.name=='nt':_ctypes.FreeLibrary(handle)
        else:_ctypes.dlclose(handle)
        cls.tmp.cleanup()

    def setUp(self):
        self.lib.skin_assets_release()
        self.dir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.dir.name)
        (self.root / 'assets').mkdir()
        self.err = ctypes.create_string_buffer(128)

    def tearDown(self):
        self.lib.skin_assets_release()
        self.dir.cleanup()

    def manifest(self, assets):
        return json.dumps({'assets': assets}).encode()

    def asset(self, name='a', **kw):
        return dict(id=name, file=f'assets/{name}.png', type='image', **kw)

    def validate(self, data):
        return self.lib.skin_assets_validate_manifest(str(self.root).encode(), data, self.err, len(self.err))

    def install(self, names):
        (self.root / 'skin.json').write_bytes(self.manifest([self.asset(n) for n in names]))
        for n in names:
            (self.root / f'assets/{n}.png').write_bytes(png())
        self.lib.skin_assets_set_root(str(self.root).encode())

    def draw(self, name='a', x=0, y=0, w=100, h=100, alpha=200):
        return self.lib.skin_assets_image(name.encode(), x, y, w, h, alpha)

    def test_manifest_restrictions(self):
        self.assertEqual(self.validate(b'{}'), 1)
        self.assertEqual(self.validate(self.manifest([self.asset()])), 1)
        for path in ['../x.png', 'assets/../x.png', 'assets/a\\b.png', 'assets/中文.png', 'assets/.png', 'assets/a.jpg', 'assets/' + 'a'*90 + '.png']:
            a = self.asset(); a['file'] = path
            self.assertEqual(self.validate(self.manifest([a])), 0, path)
        for data in [b'{"assets":{}}', b'{"assets":[{}]}', b'{"assets":[null]}', self.manifest([self.asset(), self.asset()]), self.manifest([self.asset(str(n)) for n in range(13)]), b'{"assets":[],"assets":[]}', b'{"assets":[{"id":"a","id":"b","type":"image","file":"assets/a.png"}]}']:
            self.assertEqual(self.validate(data), 0, data)

    def test_probe_checks_file_crc_dimensions_and_size(self):
        self.install(['a'])
        probe = lambda: self.lib.skin_assets_probe(str(self.root).encode(), self.err, len(self.err))
        self.assertEqual(probe(), 1)
        content = bytearray(png()); content[-1] ^= 1
        for content in [content, png(513, 1), b'x' * (128*1024+1), b'']:
            (self.root / 'assets/a.png').write_bytes(content)
            self.assertEqual(probe(), 0)
        (self.root / 'assets/a.png').unlink()
        self.assertEqual(probe(), 0)

    def test_cache_defer_lru_alpha_and_release(self):
        self.install(['a','b','c'])
        self.assertEqual(self.draw('a'), 1)
        self.assertEqual(ctypes.c_int.in_dll(self.lib, 'last_alpha').value, 200)
        self.assertEqual(ctypes.c_int.in_dll(self.lib, 'source_alpha').value, 128)
        self.assertEqual(self.draw('b'), 0)
        self.lib.skin_assets_begin_frame()
        self.assertEqual(self.draw('b'), 1)
        self.assertEqual(self.draw('a'), 1)
        self.lib.skin_assets_begin_frame()
        self.assertEqual(self.draw('c'), 1)
        self.assertEqual(self.draw('a'), 1)
        self.assertEqual(self.draw('b'), 0)
        self.lib.skin_assets_release()
        self.assertEqual(self.draw('a'), 0)
        self.assertEqual(ctypes.c_int.in_dll(self.lib, 'allocs').value, ctypes.c_int.in_dll(self.lib, 'frees').value)

    def test_full_size_512_rgba_background_decodes_with_external_workspace(self):
        self.install(['a'])
        (self.root/'assets/a.png').write_bytes(png(512,512))
        self.lib.skin_assets_set_root(str(self.root).encode())
        self.assertEqual(self.draw('a',w=480,h=272),1)
        self.assertEqual(ctypes.c_int.in_dll(self.lib,'source_alpha').value,128)

    def test_invalid_geometry_and_negative_cache(self):
        self.install(['a'])
        for kwargs in [dict(x=-481),dict(y=545),dict(w=961),dict(h=-1),dict(alpha=256)]:
            self.assertEqual(self.draw(**kwargs), -1)
        self.assertEqual(self.draw('../a'), -1)
        self.assertEqual(self.draw('missing'), 0)
        (self.root / 'assets/a.png').write_bytes(b'bad')
        self.assertEqual(self.draw(), -1)
        (self.root / 'assets/a.png').write_bytes(png())
        self.lib.skin_assets_begin_frame()
        self.assertEqual(self.draw(), -1)
        self.lib.skin_assets_set_root(str(self.root).encode())
        self.assertEqual(self.draw(), 1)


if __name__ == '__main__':
    unittest.main()
