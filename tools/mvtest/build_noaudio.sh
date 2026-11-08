#!/usr/bin/env bash
# Build the CPU-only MV decoder smoke test with WiliMini's pspvideo.
set -e

PSPDEV=${PSPDEV:-/e/pspdev}
export PSPDEV
export PATH="$PSPDEV/bin:/usr/bin:/bin"

WILI=/e/WiliMini
FF=$WILI/build/ffmpeg-audio-install
SRC=$WILI/decoder/src
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/build_noaudio
mkdir -p "$OUT"

CFLAGS="-O2 -G0 -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
 -I$PSPDEV/psp/sdk/include \
 -include $HERE/build_id.h \
 -I$WILI/decoder/include -I$SRC -I$WILI/decoder/me -I$FF/include \
 -I$WILI/build/ffmpeg-source -I$WILI/build/ffmpeg-source/libavcodec \
 -D_POSIX_C_SOURCE=200809L -DPSP_VIDEO_LIBRARY=1 \
 -DPSP_INTEGER_UNALIGNED_COPY=0 -DPSP_UNROLLED_AVG=0 \
 -DPSP_AUDIO=0 -DPSP_AUDIO_FIXED=0 -DPSP_AUDIO_VFPU=1 -DPSP_AUDIO_ADAPTIVE=1 \
 -DPSP_AUDIO_VIDEO_LEAD_US=800000 -DPSP_AUDIO_PRESENT_IMMEDIATE=1 -DPSP_AUDIO_VERIFY=0 \
 -DPSP_PTS_PACE=1 -DPSP_RGB_PACE=1 -DPSP_RGB_NEAREST_PTS=1 \
 -DPSP_VFPU_DSP=4 -DPSP_VFPU_RENDER=0 -DPSP_HALF_RENDER=0 \
 -DPSP_YUV_LUT=1 -DPSP_SPATIAL_LUT=1 -DPSP_GROUPED_RENDER=1 -DPSP_CLIP_LUT=1 -DPSP_PAIRED_ROWS=1 \
 -DPSP_PACKED_CHROMA=1 -DPSP_APPROX_QPEL=2 -DPSP_SKIP_FRAME=8 -DPSP_SKIP_LOOP_FILTER=48 \
 -DPSP_ME_RENDER=0 -DPSP_ME_DMA=0 -DPSP_ME_CACHE=0 -DPSP_ME_FULL_CACHE=0 \
 -DPSP_ME_RENDER_QUEUE=0 -DPSP_ME_DECODE=0 -DPSP_ME_DECODE_RENDER=0 \
 -DPSP_HEADLESS_BENCH=0 -DPSP_BENCH_FULL=0 -DPSP_HEADLESS_PROFILE=0 -DPSP_HEADLESS_VERIFY=0"

for f in player vfpu_dsp audio_vfpu; do
    psp-gcc $CFLAGS -c "$SRC/$f.c" -o "$OUT/$f.o"
done
psp-ar rcs "$OUT/libpspvideo_noaudio.a" "$OUT/player.o" "$OUT/vfpu_dsp.o" "$OUT/audio_vfpu.o"

psp-gcc $CFLAGS -c "$HERE/main.c" -o "$OUT/main.o"
psp-gcc -G0 -Wl,-zmax-page-size=128 -o "$OUT/mvtest.elf" "$OUT/main.o" "$OUT/libpspvideo_noaudio.a" \
    -L"$PSPDEV/psp/sdk/lib" -L"$PSPDEV/psp/lib" \
    -Wl,--start-group "$FF/lib/libavformat.a" "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a" -Wl,--end-group \
    -Wl,--wrap=ff_h264qpel_init -Wl,--wrap=ff_h264chroma_init -Wl,--wrap=avpriv_float_dsp_alloc \
    -lpspdisplay -lpspge -lpspdmac -lpspaudio -lpspctrl -lpsppower -lpspdebug -lpsputility -lm

cp "$HERE/PARAM.SFO" "$OUT/PARAM.SFO"
psp-fixup-imports "$OUT/mvtest.elf"
psp-strip "$OUT/mvtest.elf" -o "$OUT/mvtest_strip.elf"
pack-pbp "$OUT/EBOOT.PBP" "$OUT/PARAM.SFO" NULL NULL NULL NULL NULL "$OUT/mvtest_strip.elf" NULL
echo "BUILD_OK $OUT/EBOOT.PBP"

