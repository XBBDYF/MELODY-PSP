#!/usr/bin/env python3
"""Run every self-contained host test in tests/ and report a summary.

Usage: python -B tests/run_all.py [-v]
Exit code 0 = all pass, 1 = at least one failure.
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VERBOSE = '-v' in sys.argv[1:]

tests = sorted(glob.glob(os.path.join(HERE, 'test_*.py')))
failed = []
for path in tests:
    name = os.path.basename(path)
    result = subprocess.run([sys.executable, '-B', path],
                            capture_output=True, text=True, errors='replace')
    ok = result.returncode == 0
    print(('PASS ' if ok else 'FAIL ') + name)
    if not ok:
        failed.append((name, (result.stderr or result.stdout or '').strip().splitlines()[-6:]))
    elif VERBOSE:
        tail = (result.stdout or '').strip().splitlines()
        if tail:
            print('     ' + tail[-1])

print('\n%d/%d passed' % (len(tests) - len(failed), len(tests)))
for name, tail in failed:
    print('\n--- ' + name)
    for line in tail:
        print('    ' + line)
sys.exit(1 if failed else 0)
