"""Local MV scanning is opt-in: default off, persisted, and actually gates the scan.

Field report: the MV list mixed in ms0:/VIDEO files, which is slow to stat and can
list videos the built-in decoder cannot play. The switch must default to off, warn
when enabled, and take effect without a restart.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".tmp" / "local-mv-setting-test"
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / "src/store.c").read_text(encoding="utf-8")
assert "int store_scan_local_mv(void)" in source, "local MV getter is missing"
actual = source[source.index("#define STORE_PATH"):source.index("const Song *store_recent_get")]

prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "search_history.h"
#undef APP_DIR
#define APP_DIR ""
#define io_lock() ((void)0)
#define io_unlock() ((void)0)
#define mlog(...) ((void)0)
'''
tail = r'''
static void fixture(const char *text) {
    FILE *fp = fopen("data.txt", "w"); assert(fp); fputs(text, fp); fclose(fp);
}
int main(void) {
    /* Default is off, including for old data.txt that has no L record. */
    remove("data.txt"); store_init(); assert(store_scan_local_mv() == 0);
    fixture("R|1|1|a|b|0\nT|1|0|-|-\n"); store_init();
    assert(store_scan_local_mv() == 0);
    /* Persisted as its own L record and restored. */
    store_set_scan_local_mv(1);
    assert(store_scan_local_mv() == 1);
    { FILE *fp = fopen("data.txt", "r"); assert(fp); char d[2048]; size_t n = fread(d,1,sizeof(d)-1,fp);
      d[n] = 0; fclose(fp); assert(strstr(d, "L|1|0|-|-\n")); }
    store_init(); assert(store_scan_local_mv() == 1);
    store_set_scan_local_mv(0); store_init(); assert(store_scan_local_mv() == 0);
    /* Strict values: only "1" enables. */
    const char *invalid[] = {"0", "2", "-1", "junk", "1junk", "01", " 1", ""};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        char line[80]; snprintf(line, sizeof(line), "L|%s|0|-|-\n", invalid[i]);
        fixture(line); store_init(); assert(store_scan_local_mv() == 0);
    }
    puts("PASS local MV switch: default off, L record roundtrip, strict values");
    return 0;
}
'''
(OUT / "test.c").write_text(prefix + actual + tail, encoding="utf-8")
exe = OUT / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "src"), str(OUT / "test.c"), str(ROOT / "src/search_history.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], cwd=OUT, check=True)

# The scan must be gated and the toggle must refresh it plus show the warning.
mv = (ROOT / "src/mv.c").read_text(encoding="utf-8")
assert "if(!store_scan_local_mv())return;" in mv, "local scan is not gated by the setting"
assert "void mv_local_video_rescan(void)" in mv and "local_videos_scanned=0" in mv
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
handler = main[main.index("static void input_settings(unsigned int"):]
handler = handler[:handler.index("if (btn_repeat")]
assert "setting==24" in handler and "mv_local_video_rescan()" in handler
assert "不推荐" in handler, "enabling local scanning must warn the user"
ui = (ROOT / "src/ui.c").read_text(encoding="utf-8")
assert '{1,"本地 MV 搜索"}' in ui and "只显示下载的 MV" in ui
print("PASS local MV setting input, warning, rescan hook and UI label")
