"""XMB2 coherent music navigation, dedicated views and compatibility contracts."""
from pathlib import Path
import unittest,json
R=Path(__file__).resolve().parents[1];F=R/'skin_release/melody/skins/xmb'
class XmbThemeContract(unittest.TestCase):
 @classmethod
 def setUpClass(cls):cls.lua=(F/'skin.lua').read_text(encoding='utf-8')
 def test_template_and_size(self):self.assertEqual(self.lua,(R/'tools/skin_templates/xmb.lua').read_text(encoding='utf-8'));self.assertLess(len(self.lua.encode('utf-8')),49152)
 def test_music_categories_replace_system_services(self):
  head=self.lua.split('-- Each silhouette',1)[0]
  for name in ['音乐','曲库','搜索','下载','设置']:self.assertIn('name="'+name+'"',head)
  self.assertNotIn('name="PlayStation Network"',head);self.assertIn('discovery:-1',head);self.assertIn('cache:0',head)
 def test_folder_navigation_not_hidden_tabs(self):
  for token in ['folderStack','transferFolder','offlineFolder','xmb_back','xmb_open','downloads:','cache:','discovery:']:self.assertIn(token,self.lua)
  self.assertIn('directory("下载"',self.lua);self.assertIn('currentFolder',self.lua)
 def test_dedicated_player_and_coherent_keyboard(self):
  a=self.lua.index('local function xmbPlayer');b=self.lua.index('local function xmbSearch',a);player=self.lua[a:b];self.assertNotIn('playerMode',player);self.assertNotIn('detailPanel',player);self.assertIn('音乐 / 正在播放',player);self.assertIn('m.disc_player==1 and ui.disc',player);self.assertIn('ui.music_fx',player);self.assertIn('SELECT 菜单',player)
  search=self.lua[b:self.lua.index('local function xmbRows',b)];self.assertNotIn('"#FFFFFF"',search);self.assertIn('m.search_edit',search);self.assertIn('m.search_kind',search)
 def test_palette_and_effect_options(self):
  m=json.loads((F/'skin.json').read_text(encoding='utf-8'));p={x['id']:x for x in m['preferences']};self.assertEqual(len(p['color_style']['options']),6);self.assertEqual(p['music_fx']['options'],['关闭','雨点','音波']);self.assertIn('if app.option then',self.lua);self.assertIn('if ui.wave then',self.lua)
 def test_footer_single_line(self):
  body=self.lua.split('local function bar(',1)[1].split('local function row(',1)[0];self.assertEqual(body.count('text('),1);self.assertNotIn('239',body)
 def test_native_fallback_and_existing_shortcuts(self):
  self.assertIn('if m.menu_open==1 then return false',self.lua);self.assertIn('(k.held&k.select)~=0 and(k.pressed&k.l)~=0 then return false',self.lua);self.assertIn('navigate("lyrics")',self.lua);self.assertTrue(self.lua.rstrip().endswith('return skin'))
if __name__=='__main__':unittest.main()
