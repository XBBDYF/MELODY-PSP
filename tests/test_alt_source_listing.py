"""Songs without an official URL stay in the list and play through alt sources.

Field report: searching 周杰伦 briefly listed dozens of tracks, then the list emptied.
Cause: the playability check marks them playable=2, and two places compacted the
list by dropping exactly those rows. They must stay listed (tagged 三方) and be
routed to the alternative sources instead of being hidden.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
main = (ROOT / "src/main.c").read_text(encoding="utf-8")
api = (ROOT / "src/api.c").read_text(encoding="utf-8")
ui = (ROOT / "src/ui.c").read_text(encoding="utf-8")
skin = (ROOT / "src/skin_runtime.c").read_text(encoding="utf-8")

# 1. The list copy keeps every row — and actually copies them.
copy_fn = main[main.index("static void copy_list_from_api(void)"):]
copy_fn = copy_fn[:copy_fn.index("g_list_kind = g_list.kind;")]
assert "playable!=2" not in copy_fn and "playable != 2" not in copy_fn, \
    "copy_list_from_api still drops songs that have no official URL"
assert "g_list.songs[i] = src->songs[i]" in copy_fn, \
    "copy_list_from_api no longer copies rows (count set but songs[] stale -> empty list)"

# 2. The check result updates playable in place instead of compacting the list.
check_ev = main[main.index("case EV_CHECK_DONE:"):]
check_ev = check_ev[:check_ev.index("case EV_PLAY_OK:")]
assert "playable==2)continue" not in check_ev, "EV_CHECK_DONE still compacts the list"
assert "g_list.songs[i].playable = api_result()->songs[j].playable;" in check_ev

# 3. No-copyright playback asks the alternative sources first, official lookup second.
play_fn = api[api.index("static void job_play(int index, const Song *song)"):]
play_fn = play_fn[:play_fn.index("static void lyric_cb(")]
alt_at = play_fn.index('if (song->playable == 2)')
first_alt = play_fn.index("alt_resolve(", alt_at)
official_after = play_fn.index("have = resolve_url(song, &ctx, err, sizeof(err));", first_alt)
assert alt_at < first_alt < official_after, "playable=2 does not try alt sources before the official API"
assert "无版权，已从第三方音源获取" in play_fn

# 4. The native row tags such songs instead of greying them out.
row = ui[ui.index("static void song_row("):]
row = row[:row.index("static void screen_list")]
assert '"三方"' in row and "playable == 2" in row

# 5. Themes can draw the same tag: the model exposes playable.
assert 'field_i(L,"playable",s->playable)' in skin
for theme in ("metro", "xmb"):
    lua = (ROOT / "skin_release/melody/skins" / theme / "skin.lua").read_text(encoding="utf-8")
    assert "playable==2" in lua, f"{theme} does not tag no-copyright rows"
print("PASS no-copyright rows stay listed, tagged 三方, and play via alt sources first")
