"""Host decoder regression: compile a DLL and compare real covers to Pillow.
Run with Python + Pillow, gcc on PATH (existing host tools only).
"""
import ctypes
from pathlib import Path
import subprocess
import sys
import os
import shutil
os.add_dll_directory(str(Path(shutil.which('gcc')).parent))
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'src/png.c'
dll = HERE / ('old.dll' if len(sys.argv) > 1 else 'current.dll')
subprocess.run(['gcc', '-shared', '-O2', '-Wall', '-Wextra', '-I' + str(ROOT / 'src'), str(source), '-lz', '-o', str(dll)], check=True)
lib = ctypes.CDLL(str(dll))
f = lib.png_decode_rgba_scaled
f.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
f.restype = ctypes.c_int
failed = 0
for path in sorted(HERE.glob('*.png')):
    data = path.read_bytes()
    image = Image.open(path).convert('RGBA')
    for maxdim in (192, 64):
        iw, ih = image.size
        dw, dh = max(1, iw * maxdim // max(iw, ih)), max(1, ih * maxdim // max(iw, ih))
        if max(iw, ih) <= maxdim:
            dw, dh = iw, ih
        # Reference pixels come from Pillow; integer box averaging follows the
        # public scaler's existing floor convention, including all RGBA channels.
        pixels = image.load()
        expected = bytearray()
        for y in range(dh):
            for x in range(dw):
                box = [pixels[xx, yy] for yy in range(y*ih//dh, (y+1)*ih//dh)
                       for xx in range(x*iw//dw, (x+1)*iw//dw)]
                expected.extend(sum(p[c] for p in box)//len(box) for c in range(4))
        w, h = ctypes.c_int(), ctypes.c_int()
        capacity = maxdim * maxdim * 4
        out = ctypes.create_string_buffer(capacity + 32)
        ctypes.memset(out, 0xA5, len(out))
        rc = f(data, len(data), out, maxdim, ctypes.byref(w), ctypes.byref(h))
        good = rc == 0 and (w.value, h.value) == (dw, dh) and out.raw[:dw*dh*4] == expected
        good &= out.raw[capacity:] == b'\xa5' * 32
        print(path.name, maxdim, 'PASS' if good else 'FAIL', 'rc', rc, 'dimensions', w.value, h.value, flush=True)
        failed += not good
    for label, invalid in [('truncated', data[:-1]), ('bad_crc', data[:29] + bytes([data[29] ^ 1]) + data[30:])]:
        rc = f(invalid, len(invalid), out, 64, ctypes.byref(w), ctypes.byref(h))
        good = rc == -1
        print(path.name, label, 'PASS' if good else 'FAIL', flush=True)
        failed += not good
raise SystemExit(bool(failed))
