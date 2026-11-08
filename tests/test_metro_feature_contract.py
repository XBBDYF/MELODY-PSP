"""Functional skin contracts: merged cache track, modal ownership, titles and same Day logic."""
from pathlib import Path
import re,json,unittest
R=Path(__file__).resolve().parents[1];S=R/'skin_release/melody/skins'
class MetroFeatureContract(unittest.TestCase):
 def test_day_is_same_behavior_not_a_separate_unmaintained_theme(self):
  night=(S/'metro/skin.lua').read_text(encoding='utf-8');day=(S/'metro_day/skin.lua').read_text(encoding='utf-8')
  self.assertEqual(day,night);self.assertIn("color_mode",night);self.assertFalse(json.loads((S/'metro_day/skin.json').read_text(encoding='utf-8'))['dark'])
 def test_buffer_layer_is_same_track_and_hidden_at_completion(self):
  lua=(S/'metro/skin.lua').read_text(encoding='utf-8');a=lua.index('local function detailPanel(');b=lua.index('local function songs(',a);player=lua[a:b]
  self.assertIn('m.cache_percent<100',player);self.assertIn('ui.rect(x,170,271,3,C.line)',player);self.assertIn('ui.rect(x,170,271*math.min(100,m.cache_percent)/100,3',player);self.assertNotIn('ui.rect(x,149',player)
 def test_native_menu_is_global_and_comments_preserve_height_engine(self):
  ui=(R/'src/ui.c').read_text(encoding='utf-8');draw=ui[ui.index('void ui_draw(void)'):ui.index('/* Pending MV')];self.assertIn('if (g_app.screen == SC_PLAYER && g_app.menu_open) player_menu_draw();',draw)
  cm=ui[ui.index('static void comment_header_draw('):ui.index('#define MINE_ITEMS')];self.assertIn('comment_list_height',cm);self.assertIn('comment_row_height',cm);self.assertIn('api_comment_replies',cm);self.assertIn('gfx_fill_rect(0,0,SCR_W,top,C_PANEL)',cm.replace(' ',''))
 def test_lyrics_translates_all_visible_lines_and_wraps(self):
  lua=(S/'metro/skin.lua').read_text(encoding='utf-8');a=lua.index('local function drawLyrics(');b=lua.index('local function lyricPanel(',a);body=lua[a:b];self.assertIn('m.lyric_translation==1',body);self.assertIn('wrapLines(r.translation,width,0)',body);self.assertIn('l.translation',body)
if __name__=='__main__':unittest.main()
