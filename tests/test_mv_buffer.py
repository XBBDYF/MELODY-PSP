"""Compile actual buffering policy and assert low/high-water hysteresis."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/mv-buffer-test';O.mkdir(exist_ok=True)
s=(R/'src/mv.c').read_text(encoding='utf-8');a=s.index('long long mv_stream_prefetch(void)');b=s.index('static int src_buffering_callback',a)
prefix=r'''
#include <assert.h>
#include <stdint.h>
#define mlog(...) ((void)0)
#define mv_stream_available() (input.available)
#define mv_stream_total() (input.total)
#include <stdio.h>
typedef struct{long long available,total;int done,error,cancel;}MvSourceSnapshot;
static MvSourceSnapshot input;
static int g_src_duration_ms,g_use_source=1,g_src_buffering,g_dur_ms,g_pos_ms,seconds=8,g_src_segmented,g_range_busy;
static long long g_src_read_cursor;
static int mv_buffer_seconds(void){return seconds;}
static MvSourceSnapshot src_snapshot(void){return input;}
'''
tail=r'''
int main(void){input=(MvSourceSnapshot){262144,10485760,0,0,0};g_src_duration_ms=80000;assert(mv_stream_prefetch()==1048576);g_src_read_cursor=240000;assert(mv_stream_buffering());input.available=524288;assert(mv_stream_buffering());input.available=1400000;assert(!mv_stream_buffering());/* Resuming needs half the target now: a 400KB buffer keeps waiting instead of playing 3s and stopping again. */input.available=400000;assert(mv_stream_buffering());input.available=1400000;assert(!mv_stream_buffering());g_dur_ms=80000;g_pos_ms=10000;input.available=1320000;assert(mv_stream_buffering());input.available=2500000;assert(!mv_stream_buffering());seconds=30;assert(mv_stream_prefetch()==3932160);input.done=1;assert(!mv_stream_buffering());input.done=0;g_use_source=0;assert(!mv_stream_buffering());g_use_source=1;seconds=8;src_seek_begin(0,60000);assert(g_pos_ms==60000&&g_src_segmented);g_range_busy=1;assert(mv_stream_buffering());g_range_busy=0;assert(mv_stream_buffering());long long floor=input.total*60000/80000;assert(g_src_read_cursor==floor);g_pos_ms=1000;input.available=floor+mv_stream_prefetch()-1;assert(mv_stream_buffering());input.available++;assert(!mv_stream_buffering());input.available=floor+100000;assert(mv_stream_buffering());input.available=floor+mv_stream_prefetch();assert(!mv_stream_buffering());src_seek_begin(0,10000);input.available=3000000;assert(!mv_stream_buffering());input.done=1;assert(!mv_stream_buffering());puts("PASS sequential and post-seek buffering: Range finished does not bypass watermark; stale position cannot resume early; half-target resume watermark; cached backward seek immediate; complete file exempt");return 0;}
'''
(O/'test.c').write_text(prefix+s[a:b]+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
