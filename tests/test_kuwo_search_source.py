"""Third-party search source (Kuwo) parsing: originals come from a plain-HTTP API.

NetEase's public search index returns mostly covers/live/other-artist tracks for hot
Chinese artists (verified live: searching 周杰伦 gave no studio original in the top 10).
Kuwo's search (http://search.kuwo.cn/r.s) is plain HTTP, needs no auth, and returns the
original entries, so they are appended to the result list and played via the alt path.
The response is single-quoted pseudo-JSON, so the parser is a bounded field scan.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
alt = (ROOT / "src/alt.c").read_text(encoding="utf-8")
api = (ROOT / "src/api.c").read_text(encoding="utf-8")
main = (ROOT / "src/main.c").read_text(encoding="utf-8")

body = alt[alt.index("static void kw_unescape(char *s)"):
           alt.index("/* 0 = 未启用/失败/无结果；>0 = 写入 out 的条数 */")]

PRE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "app.h"
#define KW_RID     "'MUSICRID':'MUSIC_"
#define KW_RID_LEN ((int)sizeof(KW_RID) - 1)
static int field_int(const char *b, const char *e, const char *key)
{
    int kl = (int)strlen(key);
    const char *p = b;
    while (p + kl < e) {
        if (!strncmp(p, key, (size_t)kl)) {
            const char *q = p + kl;
            while (q < e && (*q == '\'' || *q == ':' || *q == ' ')) q++;
            if (q < e && *q >= '0' && *q <= '9') return atoi(q);
        }
        p++;
    }
    return 0;
}
/* Real response shape (trimmed): an HTML-entity title, a clean original, a DJ
 * version whose NAME is empty, and one entry without MUSICRID. */
static const char *FIXTURE =
"{'ARTISTPIC':'','abslist':["
"{'ALBUM':'十二新作','ARTIST':'周杰伦','DC_TARGETID':'474678847','DURATION':'210',"
"'MUSICRID':'MUSIC_474678847','NAME':'花海','SONGNAME':'花海&nbsp;(Live)'},"
"{'ALBUM':'','ARTIST':'周杰伦','DC_TARGETID':'474678848','DURATION':'269',"
"'MUSICRID':'MUSIC_474678848','NAME':'晴天','SONGNAME':'晴天'},"
"{'ALBUM':'x','ARTIST':'某翻唱','DC_TARGETID':'1','DURATION':'200',"
"'MUSICRID':'MUSIC_1','NAME':'','SONGNAME':'晴天 (DJ&#39;版)'},"
"{'ARTIST':'无 rid','NAME':'坏条目','SONGNAME':'坏条目'}"
"]}";
'''
MAIN = r'''
int main(void)
{
    Song out[4];
    int n = parse_search_songs(FIXTURE, out, 4);

    assert(n == 3);                       /* the rid-less entry is skipped */
    assert(!strcmp(out[0].name, "花海 (Live)"));
    assert(!strcmp(out[0].artist, "周杰伦") && !strcmp(out[0].album, "十二新作"));
    assert(out[0].id == 474678847 && out[0].dur_ms == 210000);
    assert(!strcmp(out[1].name, "晴天") && out[1].id == 474678848);
    assert(strstr(out[2].name, "'") != NULL);   /* &#39; decoded */
    for (int i = 0; i < n; ++i) {
        assert(out[i].playable == 2);   /* no NetEase URL: alt path owns playback */
        assert(out[i].fee == -1);       /* external-source marker for the UI */
    }
    puts("PASS Kuwo search rows: entity decode, ids, duration, alt markers, skip rid-less");
    return 0;
}
'''
out_dir = ROOT / ".tmp" / "kuwo-search-test"
out_dir.mkdir(parents=True, exist_ok=True)
c_file = out_dir / "test.c"
c_file.write_text(PRE + body + MAIN, encoding="utf-8")
exe = out_dir / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src"), str(c_file), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)

# Wiring: search appends the third-party rows, ranking prefers originals.
search = api[api.index("static void job_search(const char *q)"):]
search = search[:search.index("/* 搜索联想")]
assert "alt_kuwo_search_songs(" in search, "search does not consult the third-party source"
assert "if (!append" in search, "third-party rows must only be added on the first page"

score = main[main.index("static const char *const k_cover_markers[]"):]
score = score[:score.index("/* 搜索结果：原唱优先")]
assert "is_cover_version" in score and "strstr(s->artist, q)" in score
print("PASS search wiring: Kuwo rows appended once, ranking prefers matching artist")
