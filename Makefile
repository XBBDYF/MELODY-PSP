TARGET = melody
OBJS = src/gfx_gu.o src/main.o src/gfx.o src/net.o src/json.o src/api.o src/audio.o \
       src/dl.o src/cache.o src/ui.o src/store.o src/log.o src/png.o src/jpegdec.o src/coverjpeg.o src/qr.o \
       src/local.o src/sysinfo.o src/font_data.o src/mv.o src/alt.o src/mp3dur.o src/mvexport.o src/community.o src/ime.o src/search_history.o src/search_chips.o src/skin_font.o src/power_runtime.o src/ultra_player.o src/wifi_dialog.o src/ime_pack.o src/ime_update.o src/overclock_bridge.o src/overclock_dialog.o src/platform_plugins.o

LUA_OBJS = third_party/lua-5.4.7/lapi.o third_party/lua-5.4.7/lcode.o third_party/lua-5.4.7/lctype.o third_party/lua-5.4.7/ldebug.o third_party/lua-5.4.7/ldo.o third_party/lua-5.4.7/ldump.o third_party/lua-5.4.7/lfunc.o third_party/lua-5.4.7/lgc.o third_party/lua-5.4.7/llex.o third_party/lua-5.4.7/lmem.o third_party/lua-5.4.7/lobject.o third_party/lua-5.4.7/lopcodes.o third_party/lua-5.4.7/lparser.o third_party/lua-5.4.7/lstate.o third_party/lua-5.4.7/lstring.o third_party/lua-5.4.7/ltable.o third_party/lua-5.4.7/ltm.o third_party/lua-5.4.7/lundump.o third_party/lua-5.4.7/lvm.o third_party/lua-5.4.7/lzio.o third_party/lua-5.4.7/lauxlib.o third_party/lua-5.4.7/lbaselib.o third_party/lua-5.4.7/ltablib.o third_party/lua-5.4.7/lstrlib.o third_party/lua-5.4.7/lmathlib.o third_party/lua-5.4.7/lutf8lib.o
OBJS += src/lyric_motion.o src/lyric_render.o src/update_gallery.o src/skin_music_fx.o src/skin_wave.o src/skin_options.o src/skin_options_ui.o src/skin_disc.o src/skin_transition.o src/skin_page_turn.o src/skin_assets.o src/skin_sound.o src/skin_runtime.o src/skin_catalog.o $(LUA_OBJS)

CFLAGS = -Ithird_party/lua-5.4.7 -DLUA_32BITS -DLUAI_MAXCCALLS=40 -MMD -MP -O2 -G0 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fno-strict-aliasing -IE:/WiliMini/decoder/include
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

.DEFAULT_GOAL := all

LIBDIR =
LDFLAGS =

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = MELODY PSP

# network / audio / jpeg libs: new PSPSDK splits each function into its own .o,
# so they must be linked whole-archive or we get "stubs out of order".
LIBS = -Wl,--whole-archive -lpspnet_inet -lpspnet_resolver -lpsputility \
       -lpspwlan -lpspjpeg -lpspmp3 -lpspaudio -Wl,--no-whole-archive
LIBS += -lpspgu -lm -Wl,--whole-archive -lpsppower -Wl,--no-whole-archive -lpsprtc -lpspdmac -lz third_party/coverjpeg_psp/build-psp/libcoverjpeg8.a
LIBS += tools/mvtest/build/libpspvideo_cpu.a -Wl,--start-group E:/WiliMini/build/ffmpeg-audio-install/lib/libavformat.a E:/WiliMini/build/ffmpeg-audio-install/lib/libavcodec.a E:/WiliMini/build/ffmpeg-audio-install/lib/libavutil.a -Wl,--end-group -Wl,--wrap=ff_h264qpel_init -Wl,--wrap=ff_h264chroma_init -Wl,--wrap=avpriv_float_dsp_alloc

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

src/font_data.o: src/font16.bin src/font22.bin src/font12.bin src/font32.bin

# ????????????????????`-include`?????.h ?????? .c??
# ????????????????????.o ???????????????????build.mak ?????
# ??? .d ???????????????????????ake ???????.o ???????
-include $(OBJS:.o=.d)




# Lua ABI/budget flags must invalidate all VM objects, even without C edits.
$(LUA_OBJS) src/skin_runtime.o: Makefile

# Lua does bounded UI work: prefer smaller resident code over compiler unrolling.
$(LUA_OBJS): CFLAGS += -Os

# Feature routing/layout is code-size sensitive while the 7MiB MV heap is live.
src/main.o src/ui.o src/skin_runtime.o: CFLAGS += -Os
src/main.o src/ui.o: Makefile

# Tests temporarily override this header. Never reuse an endpoint-bearing object.
.PHONY: FORCE_SKIN_ENDPOINT
src/skin_catalog.o: FORCE_SKIN_ENDPOINT

# Keep optional UI features within PSP-1000 MV startup reserve.
src/gfx.o src/gfx_gu.o src/skin_wave.o src/skin_disc.o src/skin_options.o src/skin_options_ui.o: CFLAGS += -Os

# Network/control code is I/O-bound; preserve memory for AAC/video modules.
src/api.o src/net.o src/dl.o src/store.o: CFLAGS += -Os

src/update_gallery.o src/lyric_motion.o src/lyric_render.o: CFLAGS += -Os

# Hardware MP3/control shell, not software video decode loops.
src/audio.o src/cache.o src/mv.o: CFLAGS += -Os
