"""Run the real local.c scanner against fake PSP IO, using only host GCC.

Run directly: python tests/test_local_recursive.py
Or discover: python -m unittest discover -s tests -p test_local_recursive.py
Set CC to a host C compiler executable if GCC is not on PATH.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/local_recursive.c"


class LocalRecursiveTests(unittest.TestCase):
    def test_production_scanner_eleven_filesystem_scenarios(self):
        production = (ROOT / "src/local.c").read_text(encoding="utf-8-sig")
        # Extract the entire current scanner, capacity and accessors, rather
        # than maintaining a test copy of the algorithm or its constants.
        start = production.index("#define LOCAL_MAX")
        end = production.index("static int utf8_to_utf16le", start)
        template = FIXTURE.read_text(encoding="utf-8-sig")
        self.assertEqual(template.count("/* PRODUCTION_SCAN */"), 1)
        source = template.replace("/* PRODUCTION_SCAN */", production[start:end])
        compiler = shutil.which(os.environ.get("CC", "gcc"))
        self.assertIsNotNone(compiler, "Host GCC is required; set CC or add gcc to PATH")

        with tempfile.TemporaryDirectory(prefix="melody-local-recursive-") as tmp:
            probe = Path(tmp) / "scan.c"
            binary = Path(tmp) / ("scan.exe" if os.name == "nt" else "scan")
            probe.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [compiler, "-std=c99", "-O2", "-g", "-fstack-protector-all",
                 str(probe), "-o", str(binary)],
                capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=15,
            )
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(
                sum(line.startswith("PASS ") for line in ran.stdout.splitlines()),
                11, ran.stdout,
            )
            self.assertIn("11 scenarios passed", ran.stdout)
            print(ran.stdout, end="")


if __name__ == "__main__":
    unittest.main()
