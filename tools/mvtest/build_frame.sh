#!/usr/bin/env bash
# Build the CPU-only MV frame-capture test (tools/mvtest/frame_main.c) with
# WiliMini's pspvideo, for one A/B variant.
#
#   VARIANT=A SLF=48 QPEL=2 bash tools/mvtest/build_frame.sh
#   VARIANT=B SLF=0  QPEL=2 bash tools/mvtest/build_frame.sh
#   VARIANT=C SLF=0  QPEL=1 bash tools/mvtest/build_frame.sh
#   VARIANT=D SLF=0  QPEL=0 bash tools/mvtest/build_frame.sh
#
# Only PSP_SKIP_LOOP_FILTER and PSP_APPROX_QPEL change between variants; every
# other define matches the original fast CPU recipe (packed_chroma=0,
# render_native=1, me_render=0, ...). The effective values are baked into
# build_id.h and printed by the harness into cap.txt/result.txt, so a build that
# ignored SLF/QPEL is detectable on the PSP side.
set -e

PSPDEV=${PSPDEV:-/e/pspdev}
export PSPDEV
export PATH="$PSPDEV/bin:/usr/bin:/bin"

VARIANT=${VARIANT:-A}
SLF=${SLF:-48}
QPEL=${QPEL:-0}
OUTPUT565=${OUTPUT565:-0}
BGR565_LUT=${BGR565_LUT:-0}
EXACT_QPEL_FUSED=${EXACT_QPEL_FUSED:-0}
VERIFY_YUV=${VERIFY_YUV:-0}
DECODER_PROFILE=${DECODER_PROFILE:-0}

WILI=/e/WiliMini
FF=$WILI/build/ffmpeg-audio-install
SRC=$WILI/decoder/src
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/build/$VARIANT
PATCHED=$HERE/../mvd/patched/decoder/src
mkdir -p "$OUT"

cat > "$OUT/build_id.h" <<EOF
#ifndef MELODY_MV_BUILD_ID_H
#define MELODY_MV_BUILD_ID_H
#define PSP_BUILD_ID "mvab-$VARIANT slf=$SLF qpel=$QPEL"
#endif
EOF

CFLAGS="-O2 -G0 -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
 -I$PSPDEV/psp/sdk/include \
 -include $OUT/build_id.h \
 -I$HERE/../../src -I$PATCHED -I$WILI/decoder/include -I$SRC -I$WILI/decoder/me -I$FF/include \
 -I$WILI/build/ffmpeg-source -I$WILI/build/ffmpeg-source/libavcodec \
 -D_POSIX_C_SOURCE=200809L -DPSP_VIDEO_LIBRARY=1 -DPSP_OUTPUT565=$OUTPUT565 -DPSP_BGR565_LUT=$BGR565_LUT -DPSP_EXACT_QPEL_FUSED=$EXACT_QPEL_FUSED -DPSP_VERIFY_YUV=$VERIFY_YUV -DPSP_DECODER_PROFILE=$DECODER_PROFILE \
 -DPSP_INTEGER_UNALIGNED_COPY=0 -DPSP_UNROLLED_AVG=0 \
 -DPSP_AUDIO=1 -DPSP_AUDIO_FIXED=0 -DPSP_AUDIO_VFPU=1 -DPSP_AUDIO_ADAPTIVE=1 \
 -DPSP_AUDIO_VIDEO_LEAD_US=800000 -DPSP_AUDIO_PRESENT_IMMEDIATE=1 -DPSP_AUDIO_VERIFY=0 \
 -DPSP_PTS_PACE=1 -DPSP_RGB_PACE=1 -DPSP_RGB_NEAREST_PTS=1 \
 -DPSP_VFPU_DSP=4 -DPSP_VFPU_RENDER=0 -DPSP_HALF_RENDER=0 \
 -DPSP_YUV_LUT=1 -DPSP_SPATIAL_LUT=1 -DPSP_GROUPED_RENDER=1 -DPSP_CLIP_LUT=1 -DPSP_PAIRED_ROWS=1 \
 -DPSP_NEAREST_CHROMA=0 -DPSP_PACKED_CHROMA=1 -DPSP_RENDER_NATIVE=1 -DPSP_SKIP_FRAME=8 \
 -DPSP_SKIP_LOOP_FILTER=$SLF -DPSP_APPROX_QPEL=$QPEL \
 -DPSP_ME_RENDER=0 -DPSP_ME_DMA=0 -DPSP_ME_CACHE=0 -DPSP_ME_FULL_CACHE=0 \
 -DPSP_ME_RENDER_QUEUE=0 -DPSP_ME_DECODE=0 -DPSP_ME_DECODE_RENDER=0 \
 -DPSP_HEADLESS_BENCH=0 -DPSP_BENCH_FULL=0 -DPSP_HEADLESS_PROFILE=0 -DPSP_HEADLESS_VERIFY=0"

echo "FLAGS variant=$VARIANT slf=$SLF qpel=$QPEL out=$OUT"

psp-gcc $CFLAGS -c "$PATCHED/vfpu_dsp.c" -o "$OUT/vfpu_dsp.o"
for f in player audio_vfpu; do
    INPUT="$SRC/$f.c"
    if [ "$f" = player ]; then INPUT="$PATCHED/player.c"; fi
    psp-gcc $CFLAGS -c "$INPUT" -o "$OUT/$f.o"
done
psp-ar rcs "$OUT/libpspvideo_cpu.a" "$OUT/player.o" "$OUT/vfpu_dsp.o" "$OUT/audio_vfpu.o"

psp-gcc $CFLAGS -c "$HERE/frame_main.c" -o "$OUT/frame_main.o"
psp-gcc -G0 -Wl,-zmax-page-size=128 -o "$OUT/mvframe.elf" "$OUT/frame_main.o" "$OUT/libpspvideo_cpu.a" \
    -L"$PSPDEV/psp/sdk/lib" -L"$PSPDEV/psp/lib" \
    -Wl,--start-group "$FF/lib/libavformat.a" "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a" -Wl,--end-group \
    -Wl,--wrap=ff_h264qpel_init -Wl,--wrap=ff_h264chroma_init -Wl,--wrap=avpriv_float_dsp_alloc \
    -lpspdisplay -lpspge -lpspdmac -lpspaudio -lpspctrl -lpsppower -lpspdebug -lpsputility -lm

cp "$HERE/PARAM.SFO" "$OUT/PARAM.SFO"
psp-fixup-imports "$OUT/mvframe.elf"
psp-strip "$OUT/mvframe.elf" -o "$OUT/mvframe_strip.elf"
pack-pbp "$OUT/EBOOT.PBP" "$OUT/PARAM.SFO" NULL NULL NULL NULL NULL "$OUT/mvframe_strip.elf" NULL
echo "BUILD_OK $OUT/EBOOT.PBP"
