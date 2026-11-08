"""Read-only source contracts for Metro home cover ownership.

Run directly with Python; no PSP SDK, network, build or deployment required.
These checks intentionally go red while a source binding is still unfixed.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def source(relative):
    return (ROOT / relative).read_text(encoding="utf-8-sig")


def c_function(text, name):
    """Extract one ordinary C function, balancing its braces."""
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", text)
    if not match:
        raise AssertionError(f"Missing C function: {name}")
    start = text.index("{", match.start())
    depth = 0
    for position in range(start, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:position]
    raise AssertionError(f"Unclosed C function: {name}")


class HomeTileCoverContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runtime = source("src/skin_runtime.c")
        cls.api = source("src/api.c")

    def test_playing_cover_uses_playing_art(self):
        self.assertIn("api_art(", c_function(self.runtime, "skin_cover"))

    def test_explicit_tile_sources_do_not_read_current_art_or_mutable_page(self):
        body = c_function(self.runtime, "skin_tile_cover")
        for required in ('"daily"', '"recommendations"', "api_daily_preview(",
                         "api_home_cards(", "api_thumb_slot(", "api_thumb_px(",
                         "api_request_thumbs("):
            self.assertIn(required, body)
        for forbidden in ("api_art(", "api_request_art(", "g_card_model",
                          "api_result(", "api_cards(", "api_discovery_cards("):
            self.assertNotIn(forbidden, body)
        self.assertRegex(body, r"index\s*>=\s*0")
        self.assertRegex(body, r"index\s*<\s*cards->count")
        self.assertRegex(body, r"song->art\s*\[\s*0\s*\]")
        self.assertIn('{"tile_cover",skin_tile_cover}', self.runtime.replace(" ", ""))

    def test_daily_preview_is_owned_metadata_not_a_pending_pointer(self):
        self.assertRegex(source("src/api.h"), r"const\s+Song\s*\*\s*api_daily_preview\s*\(\s*void\s*\)")
        preview = c_function(self.api, "api_daily_preview")
        self.assertNotIn("g_pending", preview)
        self.assertNotIn("g_list", preview)
        job = c_function(self.api, "job_daily")
        copied = re.search(r"(\w+)\s*=\s*g_pending\.songs\s*\[\s*0\s*\]\s*;", job)
        self.assertIsNotNone(copied, "daily success must copy a Song, not keep the shared result")
        snapshot = copied.group(1)
        self.assertIn(snapshot, preview)
        self.assertIn(snapshot, c_function(self.api, "api_logout"))
        self.assertLess(job.index(copied.group(0)), job.index("post(EV_DAILY_OK"))
        declaration = re.search(r"static\s+Song\s+" + re.escape(snapshot) + r"\s*;", self.api)
        self.assertIsNotNone(declaration)
        self.assertLess(declaration.start(), self.api.index("void api_logout("),
                        "snapshot declaration must precede logout's reference")

    def test_account_avatar_uses_existing_avatar_cache(self):
        body = c_function(self.runtime, "skin_avatar")
        for required in ("api_user_avatar(", "api_avatar_slot(", "api_avatar_px(",
                         "api_request_thumbs("):
            self.assertIn(required, body)
        self.assertNotIn("api_art(", body)
        self.assertRegex(body, r"pixels\s*,\s*48\s*,\s*48")

    def test_both_metro_skins_bind_daily_and_recommendations_explicitly(self):
        for name in ("metro", "metro_day"):
            with self.subTest(skin=name):
                lua = source(f"skin_release/melody/skins/{name}/skin.lua")
                tile = lua.split("local function tileFace", 1)[1].split("local function home", 1)[0]
                self.assertIn('if ui.tile_cover then return ui.tile_cover(...)end return false',lua)
                self.assertRegex(tile, r'(?:ui\.tile_cover|tileCover)\(\s*[\"\']daily[\"\']\s*,\s*1\s*,')
                self.assertRegex(tile, r'(?:ui\.tile_cover|tileCover)\(\s*[\"\']recommendations[\"\']\s*,')
                self.assertNotRegex(tile, r"ui\.card_cover\(\s*1\s*,")
                # The only current-art use belongs to the playing tile.
                calls = [line for line in tile.splitlines() if "ui.cover(" in line]
                self.assertTrue(calls, "playing tile should retain its cover")
                for line in calls:
                    self.assertIn('b.live=="music"', line.replace(" ", ""))
                recommendation_calls = re.findall(
                    r'(?:ui\.tile_cover|tileCover)\(\s*[\"\']recommendations[\"\']\s*,\s*([^,]+),', tile)
                self.assertTrue(all(not re.fullmatch(r"\s*1\s*", value)
                                    for value in recommendation_calls),
                                "recommendations must select an index instead of pinning card 1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
