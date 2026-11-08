"""Host check of the MV streaming gate (rate vs. required bitrate).

Field log from docs evidence: a 9,399,957 byte / 260,560 ms MV needs ~36KB/s but
the stick link delivered 17-105KB/s in bursts, so streaming started after a 289KB
buffer and then re-entered the buffer-wait loop for the whole film - the "MV 总是卡"
report. mv_stream_sustainable()/mv_stream_eta_ms() are what the start gate and the
connecting page now use.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".tmp/mv-gate-test"
OUT.mkdir(parents=True, exist_ok=True)

source = (ROOT / "src/mv.c").read_text(encoding="utf-8")
begin = source.index("long long mv_stream_rate_bps(void)")
end = source.index("const char *mv_stream_part_path", begin)
body = source[begin:end]

prefix = r'''
#include <assert.h>
#include <stdio.h>
typedef struct{long long available,total;int done,error,cancel;}MvSourceSnapshot;
static MvSourceSnapshot input;
static volatile long long g_src_rate_bps;
static int g_src_duration_ms,g_dur_ms;
static MvSourceSnapshot src_snapshot(void){return input;}
'''

tail = r'''
int main(void)
{
    /* No measurement, unknown length or unknown duration must not block a start. */
    input=(MvSourceSnapshot){0,0,0,0,0};
    assert(mv_stream_required_bps()==0 && mv_stream_sustainable()==1);
    input.total=9399957; g_src_duration_ms=0; g_dur_ms=0;
    assert(mv_stream_required_bps()==0 && mv_stream_sustainable()==1);

    /* The real logged MV: 9,399,957 bytes over 260,560 ms. */
    g_src_duration_ms=260560;
    assert(mv_stream_required_bps()==36075);
    g_src_rate_bps=0;                 assert(mv_stream_sustainable()==1);  /* unknown */
    g_src_rate_bps=20000;             assert(mv_stream_sustainable()==0);  /* 17-20KB/s bursts */
    g_src_rate_bps=45000;             assert(mv_stream_sustainable()==0);  /* <1.25x */
    g_src_rate_bps=46000;             assert(mv_stream_sustainable()==1);
    g_src_rate_bps=105000;            assert(mv_stream_sustainable()==1);

    /* Duration falls back to the container value when the detail response is empty. */
    g_src_duration_ms=0; g_dur_ms=260560;
    assert(mv_stream_required_bps()==36075);

    /* ETA is what lets the page say "剩余 2:05" instead of a bare spinner. */
    input.available=4000000; g_src_rate_bps=50000;
    assert(mv_stream_eta_ms()==107999);
    input.available=input.total;      assert(mv_stream_eta_ms()==0);
    input.available=1000; g_src_rate_bps=0; assert(mv_stream_eta_ms()==0);
    input.available=1000; g_src_rate_bps=50000; input.total=0; assert(mv_stream_eta_ms()==0);
    puts("PASS MV gate: required bitrate, 1.25x sustainability guard, ETA");
    return 0;
}
'''

(OUT / "test.c").write_text(prefix + body + tail, encoding="utf-8")
exe = OUT / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(OUT / "test.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
