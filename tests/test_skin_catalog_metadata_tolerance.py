"""Exercise local discovery with real C parsing; no network or card access."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "skin_catalog.h"
#include "net.h"
int http_get_expect(void) { return -1; }
int http_get(const char *url, char *out, int max, int *len, int timeout, char *err, int n) {
    (void)url; (void)out; (void)max; (void)len; (void)timeout; (void)err; (void)n;
    return -1;
}
int skin_runtime_validate(const char *root, char *err, int n) {
    (void)root; (void)err; (void)n;
    return -1; /* Discovery must never execute or validate Lua. */
}
int main(void) {
    int i;
    if (skin_catalog_load_local()) return 1;
    printf("count=%d\n", skin_catalog_count());
    for (i = 0; i < skin_catalog_count(); i++) {
        const SkinCatalogEntry *e = &skin_catalog_items()[i];
        char version[24];
        if (!e->installed || skin_catalog_local_version(e->id, version, sizeof(version))) return 2;
        printf("%s|%s|%d\n", e->id, version, e->has_script);
    }
    return 0;
}
'''


class SkinCatalogMetadataToleranceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="catalog-metadata-")
        cls.directory = Path(cls.temp.name)
        cls.app = cls.directory / "app"
        source = cls.directory / "harness.c"
        source.write_text(HARNESS, encoding="utf-8")
        cls.exe = cls.directory / "catalog.exe"
        result = subprocess.run([
            "gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-DSKIN_HOST_TEST",
            '-DAPP_DIR="' + cls.app.as_posix() + '/"', "-I", str(ROOT / "src"),
            str(source), str(ROOT / "src/skin_catalog.c"), str(ROOT / "src/json.c"),
            "-o", str(cls.exe),
        ], capture_output=True, text=True, encoding="utf-8")
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        if self.app.exists():
            shutil.rmtree(self.app)
        (self.app / "skins").mkdir(parents=True)

    def add_skin(self, manifest, script=True):
        folder = self.app / "skins" / "xmb"
        folder.mkdir()
        (folder / "skin.json").write_text(manifest, encoding="utf-8")
        if script:
            (folder / "skin.lua").write_text("error('must not run')", encoding="utf-8")

    def discover(self):
        result = subprocess.run([str(self.exe)], capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.splitlines()

    def test_actual_release_manifests_are_visible_without_remote_catalog(self):
        skins = ROOT / "skin_release/melody/skins"
        expected = []
        for manifest in skins.glob("*/skin.json"):
            data = json.loads(manifest.read_text(encoding="utf-8-sig"))
            folder = self.app / "skins" / manifest.parent.name
            folder.mkdir()
            shutil.copyfile(manifest, folder / "skin.json")
            script = data.get("script", False)
            if script:
                self.assertTrue((manifest.parent / "skin.lua").is_file())
                (folder / "skin.lua").write_text("error('must not run')", encoding="utf-8")
            expected.append(f"{data['id']}|{data.get('version', '未知')}|{int(bool(script))}")
        self.assertIn("xmb|2.2|1", expected)
        self.assertGreaterEqual(len(expected), 9)
        if any(entry.startswith("metro|") for entry in expected):
            expected = [entry for entry in expected if not entry.startswith("metro_day|")]
        actual = self.discover()
        self.assertEqual(actual[0], f"count={len(expected)}")
        self.assertEqual(set(actual[1:]), set(expected))

    def test_optional_metadata_accepts_scalar_container_and_future_shapes(self):
        self.add_skin(json.dumps({
            "schema": 1, "id": "xmb", "name": "XMB", "version": "2.2", "script": True,
            "min_app_version": "3.5.0", "optional_capabilities": ["wave", "image"],
            "preferences": [{"id": "wave_motion", "type": "toggle", "default": True}],
            "future": {"name": "unrelated", "script": "../ignored", "nested": [None, False, {}]},
            "colors": {"background": "#000000"}, "dark": True,
        }))
        self.assertEqual(self.discover(), ["count=1", "xmb|2.2|1"])

    def test_duplicate_core_fields_and_unsafe_script_stay_rejected(self):
        prefix = '{"schema":1,"id":"xmb","name":"XMB","version":"1.0",'
        for field in ['"id":"xmb","script":true', '"script":"../skin.lua"',
                      '"script":true,"script":"skin.lua"', '"schema":1,"script":true']:
            with self.subTest(field=field):
                self.setUp()
                self.add_skin(prefix + field + '}')
                self.assertEqual(self.discover(), ["count=0"])

    def test_fat_uppercase_directory_preserves_lowercase_manifest_id(self):
        self.add_skin((ROOT / "skin_release/melody/skins/xmb/skin.json").read_text(encoding="utf-8"))
        (self.app / "skins/xmb").rename(self.app / "skins/XMB-uppercase-temp")
        (self.app / "skins/XMB-uppercase-temp").rename(self.app / "skins/XMB")
        self.assertEqual(self.discover(), ["count=1", "xmb|2.2|1"])

    def test_different_directory_id_stays_rejected(self):
        self.add_skin('{"schema":1,"id":"other","name":"XMB","script":"skin.lua"}')
        self.assertEqual(self.discover(), ["count=0"])

    def test_canonical_metro_hides_legacy_day_without_deleting_files(self):
        for skin_id in ["metro", "metro_day"]:
            folder = self.app / "skins" / skin_id
            folder.mkdir()
            (folder / "skin.json").write_text(json.dumps({
                "schema": 1, "id": skin_id, "name": skin_id, "version": "3.0"
            }), encoding="utf-8")
        self.assertEqual(self.discover(), ["count=1", "metro|3.0|0"])
        self.assertTrue((self.app / "skins/metro_day/skin.json").is_file())

    def test_legacy_day_remains_visible_without_canonical_metro(self):
        folder = self.app / "skins/metro_day"
        folder.mkdir()
        (folder / "skin.json").write_text(json.dumps({
            "schema": 1, "id": "metro_day", "name": "Metro day", "version": "2.0"
        }), encoding="utf-8")
        self.assertEqual(self.discover(), ["count=1", "metro_day|2.0|0"])

    def test_required_script_must_exist(self):
        self.add_skin('{"schema":1,"id":"xmb","name":"XMB","script":"skin.lua"}', script=False)
        self.assertEqual(self.discover(), ["count=0"])


if __name__ == "__main__":
    unittest.main()
