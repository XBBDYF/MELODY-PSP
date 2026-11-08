"""Host check of the download progress contract used by dl.c and ui.c.

The old code counted bytes written instead of holes closed, so a refetched range
was added twice: a 4,148,706 byte job logged "done=4,155,726" and finished as
"4063KB", and the row could print more than 100%. The contract below is the single
place the number is shaped; these asserts break if the clamp, the 99% cap or the
"bytes implied by percent" helper drifts apart.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".tmp/dl-progress-test"
OUT.mkdir(parents=True, exist_ok=True)

SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include "dl_progress_contract.h"

int main(void)
{
    /* A refetched hole contributes nothing. */
    assert(dl_count_new_bytes(0, 4096, 0) == 4096);
    assert(dl_count_new_bytes(1000, 1000, 0) == 0);
    assert(dl_count_new_bytes(2000, 1000, 0) == 0);
    /* Nothing past the declared length is progress. */
    assert(dl_count_new_bytes(4000000, 4200000, 4148706) == 148706);
    assert(dl_count_new_bytes(-5, 10, 0) == 10);

    /* Percent never leaves 0..99 while the job is still writing. */
    assert(dl_pct_from_bytes(0, 1000, 0) == 0);
    assert(dl_pct_from_bytes(500, 1000, 0) == 50);
    assert(dl_pct_from_bytes(1000, 1000, 0) == 99);
    assert(dl_pct_from_bytes(4155726, 4148706, 0) == 99);   /* the logged case */
    assert(dl_pct_from_bytes(1000, 1000, 1) == 100);
    assert(dl_pct_from_bytes(10, 0, 0) == 0);               /* unknown length */
    assert(dl_pct_from_bytes(-1, 1000, 0) == 0);

    /* Done is clamped so a stale/over-counted total cannot leak into the UI. */
    assert(dl_clamp_done(4155726, 4148706) == 4148706);
    assert(dl_clamp_done(-1, 1000) == 0);
    assert(dl_clamp_done(500, 0) == 500);

    /* The byte label is derived from the same percentage the row prints. */
    assert(dl_bytes_from_pct(50, 4148706) == 2074353);
    assert(dl_bytes_from_pct(99, 1000) == 990);
    assert(dl_bytes_from_pct(100, 1000) == 1000);
    assert(dl_bytes_from_pct(0, 1000) == 0);
    assert(dl_bytes_from_pct(50, 0) == 0);
    /* Percent never regresses the implied byte count. */
    {
        long long total = 4148706;
        long long previous = -1;
        int pct;
        for (pct = 0; pct <= 100; ++pct) {
            long long bytes = dl_bytes_from_pct(pct, total);
            assert(bytes >= previous);
            previous = bytes;
        }
    }
    puts("PASS download progress: refetched bytes ignored, clamped to length, 99% cap while writing, label derived from percent");
    return 0;
}
'''

(OUT / "test.c").write_text(SOURCE, encoding="utf-8")
exe = OUT / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src"), str(OUT / "test.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
