"""Search ranking: originals before live/covers, and the query artist wins.

Field complaint: searching 周杰伦 listed covers first. Two causes — the NetEase index
itself is cover-heavy (now supplemented by Kuwo originals) and the old score only gave
small flat penalties. The score must clearly separate original / live / cover and boost
rows whose artist is the searched name.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
body = main[main.index("static const char *const k_cover_markers[]"):]
body = body[:body.index("/* 搜索结果：原唱优先")]

PRE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
static char g_list_title[48];
'''
MAIN = r'''
static Song make(const char *name, const char *artist, int pop, int playable, int fee)
{
    Song s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", name);
    snprintf(s.artist, sizeof(s.artist), "%s", artist);
    s.pop = pop;
    s.playable = playable;
    s.fee = fee;
    return s;
}

int main(void)
{
    Song original = make("晴天", "周杰伦", 100, 1, 0);
    Song live     = make("晴天 (Live)", "周杰伦", 100, 1, 0);
    Song cover    = make("晴天 (翻唱版)", "某翻唱", 100, 1, 0);
    Song dj       = make("晴天 (DJ版)", "某DJ", 100, 1, 0);
    Song kuwo     = make("晴天", "周杰伦", 0, 2, -1);   /* third-party row */
    Song vipleech = make("晴天", "周杰伦", 100, 2, 1);  /* no URL, no alt marker */

    snprintf(g_list_title, sizeof(g_list_title), "%s", "周杰伦");

    /* Version markers are recognised (and only those). */
    assert(is_cover_version("晴天 (Live)"));
    assert(is_cover_version("晴天 (翻唱版)"));
    assert(is_cover_version("晴天 (DJ版)"));
    assert(is_cover_version("晴天（伴奏）"));
    assert(!is_cover_version("晴天"));
    assert(!is_cover_version("稻香"));

    /* Ranking contract: original > third-party original > live > cover. */
    assert(song_score(&original) > song_score(&kuwo));
    assert(song_score(&kuwo) > song_score(&live));
    assert(song_score(&live) > song_score(&cover));
    assert(song_score(&dj) < song_score(&live));

    /* A row whose artist is the searched name beats a cover of the same title. */
    assert(song_score(&original) > song_score(&cover));
    /* A no-URL NetEase row does not outrank a playable original. */
    assert(song_score(&original) > song_score(&vipleech));

    /* Without a query the artist bonus must not fire. */
    g_list_title[0] = 0;
    assert(song_score(&original) < song_score(&kuwo) + 1000);
    puts("PASS search ranking: original > third-party > live > cover, artist match boosted");
    return 0;
}
'''
out_dir = ROOT / ".tmp" / "search-rank-test"
out_dir.mkdir(parents=True, exist_ok=True)
c_file = out_dir / "test.c"
c_file.write_text(PRE + body + MAIN, encoding="utf-8")
exe = out_dir / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src"), str(c_file), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
