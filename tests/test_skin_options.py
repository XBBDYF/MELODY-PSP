"""Targeted host tests using production skin_options.c and the real json.c."""
import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class Option(ctypes.Structure):
    _fields_ = [("id", ctypes.c_char * 33), ("label", ctypes.c_char * 64)] + [
        (key, ctypes.c_int) for key in
        ("type", "value", "default_value", "min", "max", "step", "option_count")
    ] + [("options", (ctypes.c_char * 64) * 8)]


class SkinOptionsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="skin-options-")
        cls.base = Path(cls.temp.name)
        cls.app = cls.base / "app"
        cls.app.mkdir()
        source = Path(__file__).resolve().parents[1] / "src"
        stub = cls.base / "stub.c"
        stub.write_text('#include <stdlib.h>\nstatic int locked;\n'
                        'void io_lock(void) { if (locked++) abort(); }\n'
                        'void io_unlock(void) { if (--locked) abort(); }\n'
                        'void mlog(const char *fmt, ...) { (void)fmt; }\n')
        library = cls.base / ("options.dll" if __import__("os").name == "nt" else "options.so")
        compiler = shutil.which("gcc") or shutil.which("cc")
        if not compiler:
            raise RuntimeError("A host C compiler is required")
        subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        "-DSKIN_HOST_TEST", '-DAPP_DIR="' + cls.app.as_posix() + '/"',
                        "-I", str(source), str(source / "skin_options.c"),
                        str(source / "json.c"), str(stub), "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.skin_options_load.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        cls.lib.skin_options_at.argtypes = [ctypes.c_int]
        cls.lib.skin_options_at.restype = ctypes.POINTER(Option)
        cls.lib.skin_options_value.argtypes = [ctypes.c_char_p, ctypes.c_int]
        cls.lib.skin_options_set.argtypes = [ctypes.c_int, ctypes.c_int]
        cls.lib.skin_options_adjust.argtypes = [ctypes.c_int, ctypes.c_int]

    @classmethod
    def tearDownClass(cls):
        # Windows keeps a loaded DLL open until process exit.
        if __import__("os").name == "nt":
            cls.temp._finalizer.detach()
        else:
            cls.temp.cleanup()

    def setUp(self):
        self.root = self.base / "skin"
        self.root.mkdir(exist_ok=True)
        prefs = self.app / "skin-prefs"
        if prefs.exists():
            if prefs.is_dir():
                shutil.rmtree(prefs)
            else:
                prefs.unlink()
        self.lib.skin_options_reset()

    def load(self, items=None, skin="one", raw=None):
        if raw is None:
            raw = json.dumps({"preferences": items}, ensure_ascii=False)
        (self.root / "skin.json").write_text(raw, encoding="utf-8")
        self.lib.skin_options_load(str(self.root).encode(), skin.encode())

    @staticmethod
    def item(kind="toggle", **kw):
        return dict(id="tile", label="布局", type=kind, **kw)

    def value(self, key="tile"):
        return self.lib.skin_options_value(key.encode(), -99)

    def test_toggle_persists_and_ids_are_namespaced(self):
        self.load([self.item()])
        self.assertEqual(self.lib.skin_options_adjust(0, 1), 0)
        self.assertEqual(self.value(), 1)
        self.load([self.item()], "two")
        self.assertEqual(self.value(), 0)
        self.load([self.item()], "one")
        self.assertEqual(self.value(), 1)
        self.assertFalse((self.root / "one.txt").exists())
        self.assertEqual((self.app / "skin-prefs/one.txt").read_text(), "tile=1\n")

    def test_choice_range_and_validation(self):
        self.load([self.item("choice", options=["标准", "合并"]),
                   dict(id="size", label="大小", type="range", min=-10, max=20, step=5, default=5)])
        self.assertEqual(self.lib.skin_options_count(), 2)
        self.assertEqual(self.lib.skin_options_at(0).contents.option_count, 2)
        self.assertEqual(self.lib.skin_options_adjust(0, -1), 0)
        self.assertEqual(self.value(), 1)
        self.assertEqual(self.lib.skin_options_set(1, 6), -1)
        self.assertEqual(self.lib.skin_options_set(1, 20), 0)
        self.assertEqual(self.lib.skin_options_adjust(1, 1), 0)
        self.assertEqual(self.value("size"), 20)
        self.assertEqual(self.lib.skin_options_set(-1, 0), -1)
        self.assertFalse(self.lib.skin_options_at(2))

    def test_old_missing_invalid_and_unknown_saved_values(self):
        self.load([self.item(default=1), dict(id="size", label="Size", type="range", min=0, max=20, step=5)])
        prefs = self.app / "skin-prefs"
        prefs.mkdir()
        (prefs / "one.txt").write_text("tile=garbage\nsize=999\nunknown=8\n")
        self.load([self.item(default=1), dict(id="size", label="Size", type="range", min=0, max=20, step=5)])
        self.assertEqual(self.value(), 1)
        self.assertEqual(self.value("size"), 20)
        self.assertEqual(self.value("unknown"), -99)
        (prefs / "one.txt").unlink()
        (prefs / "one.txt.old").write_text("tile=0\n")
        self.load([self.item(default=1)])
        self.assertEqual(self.value(), 0)

    def test_soft_skip_duplicate_unknown_metadata(self):
        self.load([self.item(default=1), self.item(default=0),
                   dict(id="future", label="Future", type="unknown"),
                   dict(id="valid", label="Valid", type="toggle", future={"nested": [1]})])
        self.assertEqual(self.lib.skin_options_count(), 2)
        self.assertEqual(self.value(), 1)

    def test_bad_json_roots_and_bounds_are_empty(self):
        for raw in ('{"preferences":[', '[]', '{"preferences":{}}',
                    '{"preferences":[],}', '{"preferences":[]} trailing'):
            self.load(raw=raw)
            self.assertEqual(self.lib.skin_options_count(), 0, raw)
        self.load([dict(id=str(i), label="x", type="toggle") for i in range(13)])
        self.assertEqual(self.lib.skin_options_count(), 0)
        self.load(raw='{"padding":"' + 'a' * 25000 + '","preferences":[]}')
        self.assertEqual(self.lib.skin_options_count(), 0)

    def test_bad_items_skip_and_utf8_limits(self):
        bad = [self.item("choice", options=["one"]), self.item("range", step=0),
               self.item("range", min=-1001), self.item("range", max=1001),
               self.item(default=0.5), self.item(labelX="ignored")]
        # Last item remains valid; optional unknown metadata is tolerated.
        self.load(bad)
        self.assertEqual(self.lib.skin_options_count(), 1)
        self.load([dict(id="x" * 33, label="x", type="toggle"),
                   dict(id="bad/path", label="x", type="toggle"),
                   dict(id="wide", label="中" * 22, type="toggle"),
                   dict(id="choice", label="x", type="choice", options=["中" * 17, "ok"]),
                   dict(id="good", label="中" * 21, type="toggle")])
        self.assertEqual(self.lib.skin_options_count(), 1)
        self.assertEqual(self.lib.skin_options_at(0).contents.id, b"good")

    def test_invalid_skin_ids_and_save_failure_keep_old_value(self):
        self.load([self.item()], "../escape")
        self.assertEqual(self.lib.skin_options_count(), 0)
        self.load([self.item()])
        (self.app / "skin-prefs").write_text("blocked")
        self.assertEqual(self.lib.skin_options_set(0, 1), -2)
        self.assertEqual(self.value(), 0)

    def test_successive_save_has_recoverable_backup(self):
        self.load([self.item()])
        self.assertEqual(self.lib.skin_options_set(0, 1), 0)
        self.assertEqual(self.lib.skin_options_set(0, 0), 0)
        self.assertEqual((self.app / "skin-prefs/one.txt.old").read_text(), "tile=1\n")
        self.assertFalse((self.app / "skin-prefs/one.txt.tmp").exists())


if __name__ == "__main__":
    unittest.main()
