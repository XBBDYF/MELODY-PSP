#!/usr/bin/env bash
set -euo pipefail
export PSPDEV=${PSPDEV:-/e/pspdev}
export PATH="$PSPDEV/bin:/usr/bin:/bin"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FF="$ROOT/.tmp/software-formats/install"
PATCHED="$ROOT/tools/mvd/patched/decoder/src"
WILI=/e/WiliMini
OUT="$ROOT/.tmp/software-players"
mkdir -p "$OUT"
for MODE in standard fast balanced sharp; do
  DEST="$OUT/$MODE"; mkdir -p "$DEST"
  SLF=0; QPEL=0; SKIP=0; FIT=0
  if [ "$MODE" = fast ]; then SLF=48; QPEL=2; SKIP=8; fi
  if [ "$MODE" = balanced ]; then SLF=8; fi
  if [ "$MODE" = sharp ]; then FIT=1; fi
  printf '#define PSP_BUILD_ID "melody-3.3.5-software-%s-multiformat"\n' "$MODE" > "$DEST/build_id.h"
  FLAGS="-O2 -G0 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I$PSPDEV/psp/sdk/include -include $DEST/build_id.h -I$ROOT/src -I$PATCHED -I$WILI/decoder/include -I$WILI/decoder/src -I$WILI/decoder/me -I$FF/include -I$ROOT/.tmp/software-formats/ffmpeg-source -I$ROOT/.tmp/software-formats/ffmpeg-source/libavcodec -D_POSIX_C_SOURCE=200809L -DPSP_VIDEO_LIBRARY=1 -DPSP_SOFT_MULTIFORMAT=1 -DPSP_SOFT_FIT=$FIT -DPSP_OUTPUT565=1 -DPSP_BGR565_LUT=1 -DPSP_EXACT_QPEL_FUSED=1 -DPSP_VERIFY_YUV=0 -DPSP_DECODER_PROFILE=0 -DPSP_INTEGER_UNALIGNED_COPY=0 -DPSP_UNROLLED_AVG=0 -DPSP_AUDIO=1 -DPSP_AUDIO_FIXED=0 -DPSP_AUDIO_VFPU=1 -DPSP_AUDIO_ADAPTIVE=1 -DPSP_AUDIO_VIDEO_LEAD_US=800000 -DPSP_AUDIO_PRESENT_IMMEDIATE=1 -DPSP_AUDIO_VERIFY=0 -DPSP_PTS_PACE=1 -DPSP_RGB_PACE=1 -DPSP_RGB_NEAREST_PTS=1 -DPSP_VFPU_DSP=4 -DPSP_VFPU_RENDER=0 -DPSP_HALF_RENDER=0 -DPSP_YUV_LUT=1 -DPSP_SPATIAL_LUT=1 -DPSP_GROUPED_RENDER=1 -DPSP_CLIP_LUT=1 -DPSP_PAIRED_ROWS=1 -DPSP_NEAREST_CHROMA=0 -DPSP_PACKED_CHROMA=1 -DPSP_RENDER_NATIVE=1 -DPSP_SKIP_FRAME=$SKIP -DPSP_SKIP_LOOP_FILTER=$SLF -DPSP_APPROX_QPEL=$QPEL -DPSP_ME_RENDER=0 -DPSP_ME_DMA=0 -DPSP_ME_CACHE=0 -DPSP_ME_FULL_CACHE=0 -DPSP_ME_RENDER_QUEUE=0 -DPSP_ME_DECODE=0 -DPSP_ME_DECODE_RENDER=0 -DPSP_HEADLESS_BENCH=0 -DPSP_BENCH_FULL=0 -DPSP_HEADLESS_PROFILE=0 -DPSP_HEADLESS_VERIFY=0"
  psp-gcc $FLAGS -c "$PATCHED/player.c" -o "$DEST/player.o"
  psp-gcc $FLAGS -c "$PATCHED/vfpu_dsp.c" -o "$DEST/vfpu_dsp.o"
  psp-gcc $FLAGS -c "$WILI/decoder/src/audio_vfpu.c" -o "$DEST/audio_vfpu.o"
  psp-gcc -O2 -G0 -DMV_HEAP_KB=7168 -I"$ROOT/tools/prxloader" -I"$WILI/decoder/include" -I"$PSPDEV/psp/sdk/include" -c "$ROOT/tools/prxloader/mvprx.c" -o "$DEST/mvprx.o"
  psp-gcc -nostartfiles -Wl,-q -Wl,-e,module_start -specs="$PSPDEV/psp/sdk/lib/prxspecs" -Wl,-T"$PSPDEV/psp/sdk/lib/linkfile.prx" -Wl,-zmax-page-size=128 -L"$PSPDEV/psp/lib" -L"$PSPDEV/psp/sdk/lib" -o "$DEST/player.elf" "$DEST/mvprx.o" "$DEST/player.o" "$DEST/vfpu_dsp.o" "$DEST/audio_vfpu.o" -Wl,--start-group "$FF/lib/libavformat.a" "$FF/lib/libavcodec.a" "$FF/lib/libswscale.a" "$FF/lib/libavutil.a" -lpspdebug -lpspdisplay -lpspge -lpspdmac -lpspaudio -lpspctrl -lpsppower -lm -Wl,--whole-archive -lpspnet_inet -lpspnet_resolver -lpsputility -lpspwlan -lpspjpeg -lpspmp3 -Wl,--no-whole-archive -lpsprtc -lpspnet -Wl,--end-group -Wl,--wrap=ff_h264qpel_init -Wl,--wrap=ff_h264chroma_init -Wl,--wrap=avpriv_float_dsp_alloc
  psp-fixup-imports "$DEST/player.elf"
  psp-prxgen "$DEST/player.elf" "$DEST/player.prx"
  printf 'SOFTWARE_PLAYER_READY %s\n' "$DEST/player.prx"
done
