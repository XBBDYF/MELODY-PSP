"""搜索历史与推荐搜索：宿主机回归（纯逻辑模块，无 PSP 依赖）。"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".tmp" / "search-data-test"
OUT.mkdir(parents=True, exist_ok=True)

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "search_history.h"
#include "search_chips.h"

int main(void)
{
    char exported[SEARCH_HIST_MAX][SEARCH_HIST_LEN];
    char chips[SEARCH_CHIP_MAX][SEARCH_CHIP_LEN];
    const char *loaded[] = {"晴天", "周杰伦", "夜曲"};
    const char *recent_artists[] = {"周杰伦", "邓紫棋", "周杰伦"};
    const char *fav_artists[] = {"林俊杰", "邓紫棋"};
    int n;

    /* ---- 历史 ---- */
    search_hist_init(loaded, 3);
    assert(search_hist_count() == 3);
    assert(!strcmp(search_hist_at(0), "晴天"));

    assert(search_hist_push("晴天") == 0);          /* 已在最前，无变化 */
    assert(search_hist_push("稻香") == 1);          /* 新增，插到最前 */
    assert(!strcmp(search_hist_at(0), "稻香"));
    assert(search_hist_count() == 4);

    assert(search_hist_push("夜曲") == 1);          /* 老词前移 */
    assert(!strcmp(search_hist_at(0), "夜曲"));
    assert(search_hist_count() == 4);   /* 前移不增加条数 */

    assert(search_hist_push("") == 0);
    assert(search_hist_push(NULL) == 0);

    /* 竖线/换行要被清成空格，避免破坏 data.txt 的格式 */
    assert(search_hist_push("a|b") == 1);
    assert(!strcmp(search_hist_at(0), "a b"));

    /* 溢出：最多 SEARCH_HIST_MAX 条，最老的被挤掉 */
    for (int i = 0; i < 20; ++i) {
        char q[16];
        snprintf(q, sizeof(q), "q%d", i);
        search_hist_push(q);
    }
    assert(search_hist_count() == SEARCH_HIST_MAX);
    assert(!strcmp(search_hist_at(0), "q19"));

    n = search_hist_export(exported, SEARCH_HIST_MAX);
    assert(n == SEARCH_HIST_MAX);
    assert(!strcmp(exported[0], "q19"));

    search_hist_clear();
    assert(search_hist_count() == 0);
    assert(search_hist_export(exported, SEARCH_HIST_MAX) == 0);

    /* ---- 推荐 ---- */
    n = search_chips_build(recent_artists, 3, fav_artists, 2, chips, SEARCH_CHIP_MAX);
    assert(n == SEARCH_CHIP_MAX);
    assert(!strcmp(chips[0], "周杰伦"));            /* 最近播放优先 */
    assert(!strcmp(chips[1], "邓紫棋"));
    assert(!strcmp(chips[2], "林俊杰"));            /* 收藏补位，去重 */
    /* 剩下用兜底填满，且不重复 */
    for (int i = 1; i < n; ++i)
        for (int j = 0; j < i; ++j)
            assert(strcmp(chips[i], chips[j]));

    /* 没有个人数据时全部回落到底 */
    n = search_chips_build(NULL, 0, NULL, 0, chips, 3);
    assert(n == 3);
    assert(!strcmp(chips[0], "周杰伦"));

    /* max 限制 */
    n = search_chips_build(recent_artists, 3, fav_artists, 2, chips, 2);
    assert(n == 2);

    puts("PASS search data: history dedupe/move-front/cap/sanitize/export, chips priority");
    return 0;
}
'''

c = OUT / "test.c"
c.write_text(HARNESS, encoding="utf-8")
exe = OUT / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src"), str(c),
                str(ROOT / "src/search_history.c"), str(ROOT / "src/search_chips.c"),
                "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
