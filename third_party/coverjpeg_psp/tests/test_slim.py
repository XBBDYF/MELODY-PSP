"""Execute existing wrapper tests against this exact private host source config."""
from pathlib import Path
import importlib.util, subprocess, os, ctypes, unittest, io, sys
sys.dont_write_bytecode = True
from PIL import Image
ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / '.tmp/coverjpeg-slim'
PKG = ROOT / 'third_party/coverjpeg_psp'
spec = importlib.util.spec_from_file_location('existing', ROOT / 'tools/tests/test_coverjpeg.py')
existing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(existing)
class SlimTests(existing.CoverJpegTests):
    @classmethod
    def setUpClass(cls):
        BUILD.mkdir(parents=True, exist_ok=True)
        (BUILD / 'alloc_probe.c').write_text(existing.PROBE_SOURCE)
        subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c99', '-Wall', '-Wextra', '-Werror', '-O2', '-shared', '-DCOVERJPEG_TEST', '-I'+str(PKG/'include'), str(ROOT/'src/coverjpeg.c'), str(BUILD/'alloc_probe.c'), str(PKG/'build-host/libcoverjpeg8.a'), '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free', '-o', str(BUILD/'coverjpeg-slim.dll')], check=True)
        os.add_dll_directory('D:/msys64/ucrt64/bin')
        cls.lib = ctypes.CDLL(str(BUILD/'coverjpeg-slim.dll'))
        cls.decode = cls.lib.coverjpeg_decode_rgba_scaled
        cls.decode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
        cls.lib.coverjpeg_test_peak.restype = ctypes.c_size_t
        cls.lib.coverjpeg_test_limit.argtypes = [ctypes.c_size_t]
        cls.lib.probe_live.restype = cls.lib.probe_peak.restype = ctypes.c_size_t
    def test_psp_compile(self):
        subprocess.run(['E:/pspdev/bin/psp-gcc.exe', '-Os', '-G0', '-Isrc', '-I'+str(PKG/'include'), str(ROOT/'src/coverjpeg.c'), str(PKG/'tests/link_main.c'), str(PKG/'build-psp/libcoverjpeg8.a'), '-Wl,-Map,'+str(BUILD/'slim.map'), '-o', str(BUILD/'slim.elf')], cwd=ROOT, check=True)
        result = subprocess.run(['E:/pspdev/bin/psp-nm.exe', '-u', str(BUILD/'slim.elf')], check=True, stdout=subprocess.PIPE)
        self.assertEqual(result.stdout.strip(), b'')
    def test_all_native_scaling_color_subsampling(self):
        for mode in ['RGB', 'L', 'CMYK']:
            for progressive in [False, True]:
                for subsampling in [0, 1, 2]:
                    data = self.fixture((511, 317), mode, progressive, subsampling)
                    for maximum in [192, 112, 64, 32, 1]:
                        with self.subTest(mode=mode, progressive=progressive, sub=subsampling, max=maximum):
                            code,w,h,p = self.call(data, maximum)
                            self.assertEqual(code, 0)
                            self.assertEqual((w,h), (maximum, max(1,317*maximum//511)))
    def test_rgb_encoded_jpeg(self):
        im = Image.new('RGB', (73, 51))
        im.putdata([((x*7)%256, (y*9)%256, (x+y)%256) for y in range(51) for x in range(73)])
        for progressive in [False, True]:
            stream = io.BytesIO()
            im.save(stream, 'JPEG', quality=94, keep_rgb=True, progressive=progressive)
            data = stream.getvalue()
            code,w,h,pixels = self.call(data)
            self.assertEqual((code,w,h), (0,73,51))
            oracle = Image.open(io.BytesIO(data)).convert('RGBA').tobytes()
            self.assertLessEqual(max(abs(a-b) for a,b in zip(pixels[:w*h*4],oracle)),2)
    def test_non8_header_rejected(self):
        data = bytearray(self.fixture((32, 32), progressive=False))
        i = data.index(b'\xff\xc0')
        data[i+4] = 12
        self.assertEqual(self.call(bytes(data))[:3], (-1,71,83))
if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(SlimTests))
    raise SystemExit(not result.wasSuccessful())
