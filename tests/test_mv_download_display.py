"""MV progress is computed from bytes, never from a percentage.

Field bug: the MV row printed "下载中 · 11.5/11.5MB" while the same row's bar was at
33%. The label was derived as total*pct/100, so any disagreement between the two
sources shows up as an impossible number. The row must ask for bytes directly.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".tmp" / "mv-progress-display-test"
OUT.mkdir(parents=True, exist_ok=True)

api = (ROOT / "src/api.c").read_text(encoding="utf-8")
assert "long long api_mv_download_bytes(void)" in api
body = api[api.index("long long api_mv_download_bytes(void)"):
           api.index("long long api_mv_download_total(void)")]

prefix = r'''
#include <assert.h>
#include <stdio.h>
static int g_mv_stream_job;
static long long g_mv_dl_total;
static int g_mv_dl_pct;
static long long stream_total, stream_available;
static long long mv_stream_total(void) { return stream_total; }
static long long mv_stream_available(void) { return stream_available; }
/* Same contract as api.c's api_mv_download_total(), which sits after this slice. */
static long long api_mv_download_total(void)
{
    if (g_mv_stream_job && stream_total > 0) return stream_total;
    return g_mv_dl_total;
}
'''
tail = r'''
int main(void) {
    /* Streaming: 4,000,000 of 12,006,628 bytes = 33%. Byte count and percent agree. */
    g_mv_stream_job = 1; stream_total = 12006628; stream_available = 4000000;
    assert(api_mv_download_bytes() == 4000000);
    assert(api_mv_download_pct() == 33);
    /* A published value above the declared length is clamped, never 100%. */
    stream_available = 12006628;
    assert(api_mv_download_bytes() == 12006628 && api_mv_download_pct() == 99);
    stream_available = 13000000;
    assert(api_mv_download_bytes() == 12006628 && api_mv_download_pct() == 99);
    /* Before the header arrives there is no total: 0 bytes, 0 percent. */
    stream_total = 0; stream_available = 4096;
    assert(api_mv_download_bytes() == 0 && api_mv_download_pct() == 0);
    /* Non-stream fallback keeps the sync downloader's own percent. */
    g_mv_stream_job = 0; g_mv_dl_total = 1000000; g_mv_dl_pct = 42;
    assert(api_mv_download_bytes() == 420000);
    assert(api_mv_download_pct() == 42);
    g_mv_dl_total = 0; g_mv_dl_pct = 0;
    assert(api_mv_download_bytes() == 0 && api_mv_download_pct() == 0);
    puts("PASS MV bytes: streaming bytes, clamp, unknown total, sync fallback");
    return 0;
}
'''
(OUT / "test.c").write_text(prefix + body + tail, encoding="utf-8")
exe = OUT / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(OUT / "test.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)

# The row must consume the byte API; percent-derived bytes are gone for MV.
# Anchor on the definition, not the forward declaration above screen_download.
ui = (ROOT / "src/ui.c").read_text(encoding="utf-8")
marker = "static void screen_download_mv(float t, float dt)"
mv_start = ui.index(marker, ui.index(marker) + 1)
mv_block = ui[mv_start:ui.index("static void cache_tabs", mv_start)]
assert "api_mv_download_bytes()" in mv_block
assert "fmt_done_total(sz, sizeof(sz), done_bytes, total_bytes)" in mv_block
assert "dl_bytes_from_pct" not in mv_block, "MV row still derives bytes from a percent"
print("PASS MV row uses byte API and no percent-derived byte label")
