#!/usr/bin/env bash
set -e
export PSPDEV=${PSPDEV:-/e/pspdev}
export PATH="$PSPDEV/bin:/usr/bin:/bin"
HERE=$(cd "$(dirname "$0")" && pwd)
PATCHED="$HERE/../mvd/patched/decoder/src"
LUT=${BGR565_LUT:-0}
OUT="$HERE/build/colorbench_lut$LUT"
mkdir -p "$OUT"
psp-gcc -O2 -G0 -std=gnu11 -Wall -Wextra -DPSP_CLIP_LUT=1 -DPSP_BGR565_LUT=$LUT \
 -I"$PATCHED" -I/e/WiliMini/decoder/src -I"$PSPDEV/psp/sdk/include" \
 "$HERE/bgr565_bench.c" -L"$PSPDEV/psp/sdk/lib" -L"$PSPDEV/psp/lib" \
 -lpsppower -lpspdebug -lpspdisplay -lpspge -lpspctrl -lm -o "$OUT/bench.elf"
psp-fixup-imports "$OUT/bench.elf"
psp-strip "$OUT/bench.elf" -o "$OUT/bench_strip.elf"
pack-pbp "$OUT/EBOOT.PBP" "$HERE/PARAM.SFO" NULL NULL NULL NULL NULL "$OUT/bench_strip.elf" NULL
echo "BUILD_OK $OUT/EBOOT.PBP"
