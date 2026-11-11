"""Compile extracted production input statements; production frame-edge sampling survives consumed UI input.

Run: python tests/test_skin_input_edges.py -v
No PSP SDK, Lua binary, package install, or production edits required.
The proposed variant changes only the harness's end-of-frame sample baseline.
"""
import ctypes
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RIGHT = 0x20  # PSP_CTRL_RIGHT; fits exactly in Lua's signed 32-bit integer.


def extract(source, pattern):
    match = re.search(pattern, source, re.S)
    if not match:
        raise AssertionError(f"Production input structure changed: {pattern}")
    return match.group(0)


class SkinInputEdges(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "src/main.c").read_text(encoding="utf-8")
        edge = extract(source, r"pressed\s*=\s*held\s*&\s*~g_prev_btn\s*;\s*g_prev_btn\s*=\s*held\s*;")
        pending = extract(source, r"if\s*\(g_pending_press\)\s*\{[^{}]*\}")
        consume = extract(source, r"if\([^\n]*g_app.screen!=SC_SKINS[^\n]*skin_runtime_input[^\n]*;")
        # Keep the actual consumption assignment; the harness models Metro home=true.
        consume = consume.split("skin_runtime_input", 1)[1].split("))", 1)[1]
        tail = extract(source, r"g_pending_press\s*\|=\s*\(input_buttons_translate\(pad2.Buttons\)\s*&\s*~(?:sampled_held|held)\)\s*;")
        code = """
        static unsigned input_buttons_translate(unsigned p){return p;}
        unsigned run(const unsigned *samples, unsigned frames, int handled,
                     int snapshot, unsigned *trace) {
            unsigned g_prev_btn=0, g_pending_press=0, count=0;
            for(unsigned i=0;i<frames;++i) {
                unsigned held=samples[i], pressed, raw_held=held;
                const unsigned sampled_held=held; (void)sampled_held;
                struct {unsigned Buttons;} pad2={samples[i]};
                EDGE
                PENDING
                trace[i]=pressed;
                if(pressed & 0x20u) ++count;
                if(handled) { CONSUME }
                if(snapshot) { g_pending_press |= (pad2.Buttons & ~raw_held); }
                else { TAIL }
            }
            return count;
        }
        """
        for marker, text in (("EDGE", edge), ("PENDING", pending),
                             ("CONSUME", consume), ("TAIL", tail)):
            code = code.replace(marker, text)
        cls.temp = tempfile.TemporaryDirectory(prefix="metro-input-")
        folder = Path(cls.temp.name)
        harness = folder / "edges.c"
        harness.write_text(code, encoding="utf-8")
        library = folder / "edges.dll"
        compiler = shutil.which("gcc") or shutil.which("cc")
        if not compiler:
            raise RuntimeError("An existing C compiler is required; do not install one")
        subprocess.run([compiler, "-shared", "-O0", str(harness), "-o", str(library)],
                       check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.run.argtypes = [ctypes.POINTER(ctypes.c_uint), ctypes.c_uint,
                                ctypes.c_int, ctypes.c_int,
                                ctypes.POINTER(ctypes.c_uint)]
        cls.lib.run.restype = ctypes.c_uint

    @classmethod
    def tearDownClass(cls):
        # Windows keeps a loaded DLL locked until explicitly unloaded.
        import _ctypes
        import os
        handle = cls.lib._handle
        del cls.lib
        if os.name == "nt":
            _ctypes.FreeLibrary(handle)
        else:
            _ctypes.dlclose(handle)
        cls.temp.cleanup()

    def run_frames(self, samples, handled=True, snapshot=False):
        array = (ctypes.c_uint * len(samples))(*samples)
        trace = (ctypes.c_uint * len(samples))()
        count = self.lib.run(array, len(samples), handled, snapshot, trace)
        return count, list(trace)

    def test_production_metro_press_hold_60_frames_is_one_edge(self):
        count, trace = self.run_frames([RIGHT] * 60)
        self.assertEqual(count, 1, f"Repeated pressed masks: {trace[:8]}")

    def test_unconsumed_press_hold_is_one_edge(self):
        self.assertEqual(self.run_frames([RIGHT] * 60, handled=False)[0], 1)

    def test_proposed_raw_snapshot_hold_is_one_edge(self):
        count, trace = self.run_frames([RIGHT] * 60, snapshot=True)
        self.assertEqual(count, 1)
        self.assertEqual(trace, [RIGHT] + [0] * 59)

    def test_proposed_raw_snapshot_release_and_repress_is_two_edges(self):
        samples = [RIGHT] * 30 + [0] * 3 + [RIGHT] * 30
        self.assertEqual(self.run_frames(samples, snapshot=True)[0], 2)


if __name__ == "__main__":
    unittest.main()
