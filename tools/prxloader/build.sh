#!/usr/bin/env bash
set -e
export PSPDEV=${PSPDEV:-/e/pspdev}
export PATH="$PSPDEV/bin:/usr/bin:/bin"

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/tools/prxloader
SRC=$ROOT/src
LUA_OBJS=$(sed 's/\.c$/.o/' "$OUT/lua_sources.txt" | sed "s#^#$ROOT/#" | tr '\n' ' ')
OBJS="$SRC/lyric_motion.o $SRC/lyric_render.o $SRC/update_gallery.o $SRC/skin_music_fx.o $SRC/skin_wave.o $SRC/skin_options.o $SRC/skin_options_ui.o $SRC/skin_disc.o $SRC/gfx_gu.o $SRC/skin_transition.o $SRC/skin_page_turn.o $SRC/skin_assets.o $SRC/skin_sound.o $SRC/skin_runtime.o $SRC/skin_catalog.o $LUA_OBJS $SRC/main.o $SRC/gfx.o $SRC/net.o $SRC/json.o $SRC/api.o $SRC/audio.o $SRC/dl.o $SRC/cache.o $SRC/ui.o $SRC/store.o $SRC/log.o $SRC/png.o $SRC/jpegdec.o $SRC/coverjpeg.o $SRC/qr.o $SRC/local.o $SRC/sysinfo.o $SRC/font_data.o $SRC/mv.o $SRC/alt.o $SRC/mp3dur.o $SRC/mvexport.o $SRC/community.o $SRC/ime.o $SRC/search_history.o $SRC/search_chips.o $SRC/skin_font.o $SRC/power_runtime.o $SRC/ultra_player.o $SRC/wifi_dialog.o $SRC/ime_pack.o $SRC/ime_update.o $SRC/overclock_bridge.o $SRC/overclock_dialog.o $SRC/platform_plugins.o $SRC/input_buttons.o $OUT/prx_entry.o"

psp-gcc -O2 -G0 -I"$SRC" -I"$PSPDEV/psp/sdk/include" -I/e/WiliMini/decoder/include \
    -D_PSP_FW_VERSION=600 -c "$SRC/prx_entry.c" -o "$OUT/prx_entry.o"

psp-gcc -nostartfiles -Wl,-q -Wl,-e,module_start \
    -specs="$PSPDEV/psp/sdk/lib/prxspecs" -Wl,-T"$PSPDEV/psp/sdk/lib/linkfile.prx" \
    -Wl,-zmax-page-size=128 -L"$PSPDEV/psp/lib" -L"$PSPDEV/psp/sdk/lib" \
    -o "$OUT/melody.prx.elf" $OBJS \
    -Wl,--start-group \
        -lpspgu -lpspdebug -lpspdisplay -lpspge -lpspdmac -lpspaudio -lpspctrl -Wl,--whole-archive -lpsppower -Wl,--no-whole-archive -lm \
        -Wl,--whole-archive -lpspnet_inet -lpspnet_resolver -lpsputility -lpspwlan \
        -lpspjpeg -lpspmp3 -Wl,--no-whole-archive \
        -lpsprtc -lpspnet -lpspnet_apctl -lz "$ROOT/third_party/coverjpeg_psp/build-psp/libcoverjpeg8.a" \
    -Wl,--end-group

psp-fixup-imports "$OUT/melody.prx.elf"
psp-prxgen "$OUT/melody.prx.elf" "$OUT/melody.prx"

make -C "$OUT" -f Makefile.loader EBOOT.PBP

echo BUILD_OK
