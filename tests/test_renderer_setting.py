"""Exercise the real renderer store parser/save code and stable settings wiring."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".tmp" / "renderer-setting-test"
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / "src/store.c").read_text(encoding="utf-8")
# Keep the actual fields, parser, save routine and accessors, without PSP IO.
actual = source[source.index("#define STORE_PATH"):source.index("const Song *store_recent_get")]
prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "app.h"
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
    fixture("T|1|0|-|-\nP|6|0|-|-\nB|5|0|-|-\nN|0|0|-|-\n");
    store_init();
    assert(store_renderer() == 0); /* Legacy files keep the old renderer. */
    assert(store_theme() == 1 && store_dl_parts() == 6);
    assert(g_prefetch_count == 5 && g_show_speed == 0);
    store_set_renderer(1);
    FILE *fp = fopen("data.txt", "r"); assert(fp);
    char data[1024]; size_t n = fread(data, 1, sizeof(data)-1, fp);
    data[n] = 0; fclose(fp);
    assert(strstr(data, "E|1|0|-|-\n"));
    assert(strstr(data, "T|1|0|-|-\n") && strstr(data, "P|6|0|-|-\n"));
    assert(strstr(data, "B|5|0|-|-\n") && strstr(data, "N|0|0|-|-\n"));
    g_renderer = 0; store_init(); assert(store_renderer() == 1);
    store_set_renderer(0); g_renderer = 1; store_init();
    assert(store_renderer() == 0);
    fixture("E|1|0|-|-\n"); store_init(); assert(store_renderer() == 1);
    fixture("T|0|0|-|-\n"); store_init(); assert(store_renderer() == 0);
    fixture("E|7|0|-|-\n"); store_init(); assert(store_renderer() == 0);
    store_set_renderer(-1); assert(store_renderer() == 0);
    store_set_renderer(2); assert(store_renderer() == 0);
    int seen[MELODY_SETTINGS_COUNT] = {0};
    assert(MELODY_SETTINGS_COUNT == 25);
    for (int row = 0; row < MELODY_SETTINGS_COUNT; ++row) {
        int id = melody_setting_id(row); assert(id >= 0 && id < 25);
        assert(!seen[id]++);
        if (id == 3) assert(melody_setting_id(row + 1) == 22);
    }
    assert(melody_setting_id(-1) == -1 && melody_setting_id(25) == -1);
    puts("PASS renderer legacy default, independent E record, roundtrip, binary values, stable IDs");
    return 0;
}
'''
(OUT / "test.c").write_text(prefix + actual + tail, encoding="utf-8")
exe = OUT / "test.exe"
subprocess.run([
    "D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O2", "-Wall", "-Wextra",
    "-Werror", "-I" + str(ROOT / "src"), str(OUT / "test.c"), "-o", str(exe),
], check=True)
subprocess.run([str(exe)], cwd=OUT, check=True)

main = (ROOT / "src/main.c").read_text(encoding="utf-8")
ui = (ROOT / "src/ui.c").read_text(encoding="utf-8")
app = (ROOT / "src/app.h").read_text(encoding="utf-8")
assert '#define APP_VER   "4.1.1"' in app
assert '{4,"界面渲染"}' in ui
handler = main[main.index("static void input_settings(unsigned int"):]
handler = handler[:handler.index("if (btn_repeat")]
assert re.search(r"setting\s*==\s*22", handler)
assert "PSP_CTRL_LEFT|PSP_CTRL_RIGHT|PSP_CTRL_CIRCLE" in handler
assert handler.index("gfx_set_renderer(target)==0") < handler.index("store_set_renderer(target)")
assert "gfx_renderer_error()" in handler
startup = main[main.index("    store_init();"):]
assert startup.index("gfx_set_renderer(store_renderer())") < startup.index("audio_init()")
assert "CPU软件" in ui and "GE / GU加速" in ui
print("PASS renderer setting input, startup ordering, labels and unchanged application version")


