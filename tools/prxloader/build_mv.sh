#!/usr/bin/env bash
# Build the on-demand MV PRX (8 MiB heap, audio pspvideo).
set -e
export PSPDEV=${PSPDEV:-/e/pspdev}
export PATH="$PSPDEV/bin:/usr/bin:/bin"

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/tools/prxloader
FF=/e/WiliMini/build/ffmpeg-audio-install
MV_LIB=${MV_LIB:-$ROOT/tools/mvtest/build/libpspvideo_cpu.a}
MV_OUT=${MV_OUT:-melody_mv.prx}
MV_ELF=${MV_ELF:-melody_mv.elf}

psp-gcc -O2 -G0 -I"$OUT" -I/e/WiliMini/decoder/include \
    -I"$PSPDEV/psp/sdk/include" -c "$OUT/mvprx.c" -o "$OUT/mvprx.o"

psp-gcc -nostartfiles -Wl,-q -Wl,-e,module_start \
    -specs="$PSPDEV/psp/sdk/lib/prxspecs" \
    -Wl,-T"$PSPDEV/psp/sdk/lib/linkfile.prx" -Wl,-zmax-page-size=128 \
    -L"$PSPDEV/psp/lib" -L"$PSPDEV/psp/sdk/lib" \
    -o "$OUT/$MV_ELF" "$OUT/mvprx.o" \
    -Wl,--start-group \
        "$MV_LIB" \
        "$FF/lib/libavformat.a" "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a" \
        -lpspdebug -lpspdisplay -lpspge -lpspdmac -lpspaudio -lpspctrl -lpsppower -lm \
        -Wl,--whole-archive -lpspnet_inet -lpspnet_resolver -lpsputility -lpspwlan \
        -lpspjpeg -lpspmp3 -Wl,--no-whole-archive \
        -lpsprtc -lpspnet \
    -Wl,--end-group \
    -Wl,--wrap=ff_h264qpel_init -Wl,--wrap=ff_h264chroma_init \
    -Wl,--wrap=avpriv_float_dsp_alloc

psp-fixup-imports "$OUT/$MV_ELF"
psp-prxgen "$OUT/$MV_ELF" "$OUT/$MV_OUT"
echo BUILD_OK "$OUT/$MV_OUT"
echo BUILD_OK "$OUT/melody_mv.prx"
