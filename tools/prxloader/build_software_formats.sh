#!/usr/bin/env bash
# Isolated FFmpeg build: keep the existing decoder libraries unchanged.
set -euo pipefail
export PSPDEV=${PSPDEV:-/e/pspdev}
export PATH="$PSPDEV/bin:/usr/bin:/bin"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK="$ROOT/.tmp/software-formats"
SRC="$WORK/ffmpeg-source"
PREFIX="$WORK/install"
mkdir -p "$SRC" "$PREFIX"
if [ ! -f "$SRC/configure" ]; then
  tar -C /e/WiliMini/build/ffmpeg-source --exclude='*.o' --exclude='*.a' --exclude='*.d' --exclude='config.h' --exclude='config_components.h' --exclude='config.asm' --exclude='ffbuild/config.mak' --exclude='ffbuild/config.log' --exclude='.git' -cf - . | tar -C "$SRC" -xf -
fi
cd "$SRC"
./configure --prefix="$PREFIX" --enable-cross-compile --target-os=none --arch=mips --cpu=allegrex --cross-prefix=psp- --disable-asm --disable-autodetect --disable-programs --disable-doc --disable-debug --disable-network --disable-avdevice --disable-avfilter --disable-postproc --enable-swscale --disable-swresample --disable-pthreads --disable-everything --enable-decoder=h264,mpeg4,mjpeg,flv,aac,aac_fixed,mp3float,pcm_s16le --enable-parser=h264,mpeg4video,mjpeg,mpegaudio --enable-demuxer=mov,flv,avi,matroska --enable-protocol=file --extra-cflags='-O3 -G0 -Wno-error=incompatible-pointer-types -DPSP_CABAC_MOVN=1' --extra-ldflags=-G0 --pkg-config=/usr/bin/false
make -j4
make install
printf 'SOFTWARE_FORMATS_READY %s\n' "$PREFIX"
