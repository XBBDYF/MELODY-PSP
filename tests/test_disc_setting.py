"""Compile real store settings logic; verify native disc opt-in wiring."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".tmp" / "disc-setting-test"
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / "src/store.c").read_text(encoding="utf-8")
assert "int store_disc_player(void)" in source, "disc setting getter is missing"
actual = source[source.index("#define STORE_PATH"):source.index("const Song *store_recent_get")]
prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "app.h"
#include "search_history.h"
#include "settings_order.h"
#undef APP_DIR
#define APP_DIR ""
#define io_lock() ((void)0)
#define io_unlock() ((void)0)
#define mlog(...) ((void)0)
'''
tail = r'''
static void fixture(const char *text) {
    FILE *fp = fopen("data.txt", "w"); assert(fp);
    fputs(text, fp); fclose(fp);
}
int main(void) {
    remove("data.txt"); store_init(); assert(store_disc_player() == 0);
    fixture("E|1|0|-|-\nT|1|0|-|-\nP|6|0|-|-\nB|5|0|-|-\nN|0|0|-|-\n");
    store_init(); assert(store_disc_player() == 0);
    assert(store_renderer() == 1 && store_theme() == 1 && store_dl_parts() == 6);
    assert(g_prefetch_count == 5 && g_show_speed == 0);
    store_set_disc_player(1);
    FILE *fp = fopen("data.txt", "r"); assert(fp);
    char data[2048]; size_t n = fread(data, 1, sizeof(data)-1, fp);
    data[n] = 0; fclose(fp);
    assert(strstr(data, "V|1|0|-|-\n"));
    assert(strstr(data, "E|1|0|-|-\n") && strstr(data, "T|1|0|-|-\n"));
    assert(strstr(data, "P|6|0|-|-\n") && strstr(data, "B|5|0|-|-\n"));
    assert(strstr(data, "N|0|0|-|-\n"));
    store_init(); assert(store_disc_player() == 1);
    store_set_disc_player(0); store_init(); assert(store_disc_player() == 0);
    fixture("V|1|0|-|-\n"); store_init(); assert(store_disc_player() == 1);
    fixture("E|0|0|-|-\n"); store_init(); assert(store_disc_player() == 0);
    const char *invalid[] = {"0", "2", "-1", "junk", "1junk", "01", "+1", " 1", ""};
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        char line[80]; snprintf(line, sizeof(line), "V|%s|0|-|-\n", invalid[i]);
        fixture(line); store_init(); assert(store_disc_player() == 0);
    }
    store_set_disc_player(2); assert(store_disc_player() == 0);
    store_set_disc_player(-1); assert(store_disc_player() == 0);
    store_set_disc_player(1); remove("data.txt"); store_init(); assert(store_disc_player() == 0);
    int seen[MELODY_SETTINGS_COUNT] = {0};
    assert(MELODY_SETTINGS_COUNT == 25);
    for (int row=0; row<MELODY_SETTINGS_COUNT; ++row) {
        int id=melody_setting_id(row); assert(id>=0 && id<25); assert(!seen[id]++);
        if(id==3) assert(melody_setting_id(row+1)==22);
        if(id==22) assert(melody_setting_id(row+1)==23);
    }
    assert(melody_setting_id(-1)==-1 && melody_setting_id(25)==-1);
    puts("PASS real store default/legacy/strict values/roundtrip and stable IDs");
    return 0;
}
'''
(OUT / "test.c").write_text(prefix + actual + tail, encoding="utf-8")
exe = OUT / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"), str(OUT / "test.c"), str(ROOT / "src/search_history.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], cwd=OUT, check=True)
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
ui = (ROOT / "src/ui.c").read_text(encoding="utf-8")
app = (ROOT / "src/app.h").read_text(encoding="utf-8")
assert '#define APP_VER   "4.1.1"' in app
assert '{4,"唱片播放器"}' in ui
handler = main[main.index("static void input_settings(unsigned int"):]
handler = handler[:handler.index("if (btn_repeat")]
assert re.search(r"setting\s*==\s*23", handler)
assert "PSP_CTRL_LEFT|PSP_CTRL_RIGHT|PSP_CTRL_CIRCLE" in handler
assert "store_set_disc_player(" in handler
assert re.search(r"if\s*\(!\s*\w+\)\s*skin_disc_release\(\)", handler)
assert 'store_disc_player()?"开":"关"' in ui
player = ui[ui.index("static void screen_player(float"):ui.index("static void screen_lyrics(float")]
assert 'if (store_disc_player())' in player
assert "skin_disc_draw(24, 54, size, disc_angle)" in player
assert "g_app.has_now && !g_app.paused && audio_state() == AUDIO_PLAY" in player
assert "disc_angle +=" in player and "dt" in player
assert "int size = 134;" in player
assert "draw_cover(24, 54, size, 8, art, aw, ah);" in player
assert "static float disc_angle" in player
print("PASS native opt-in geometry, frame rotation/playback guards, release and setting labels")


