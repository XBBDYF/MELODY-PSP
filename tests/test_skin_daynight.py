"""Data regressions for the restored original day/night palettes."""
import ast
import json
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
SKINS = ROOT / 'skin_release/melody/skins'
TOKENS = 'background panel text secondary muted line accent hot panel2 white black success warning row shadow'.split()
EXPECTED = {
    'daylight': '#EFEFEF #FBFBFB #222222 #666666 #9A9A9A #E3E3E3 #C20C0C #EF2727 #F1F1F1 #FFFFFF #000000 #2BA471 #E04B4B #ECECEC #9A9A9A'.split(),
    'night': '#101012 #18181A #EAEAEA #A2A2A8 #707078 #2D2D31 #EC4141 #FF5F5F #242427 #FFFFFF #000000 #3ECF8E #FF6B6B #26262A #000000'.split(),
}


class RestoredSkinTests(unittest.TestCase):
    def test_exact_original_palettes_and_metadata(self):
        for skin_id, values in EXPECTED.items():
            with self.subTest(skin_id=skin_id):
                folder = SKINS / skin_id
                meta = json.loads((folder / 'skin.json').read_text(encoding='utf-8'))
                self.assertEqual(meta['colors'], dict(zip(TOKENS, values)))
                self.assertIs(meta['dark'], skin_id == 'night')
                self.assertEqual(meta['name'], '白天模式' if skin_id == 'daylight' else '黑夜模式')
                self.assertEqual(meta['description'], '恢复原版白天/黑夜配色')
                self.assertLessEqual(len(meta['name'].encode('utf-8')), 63)
                self.assertFalse(meta.get('script', False))
                self.assertFalse((folder / 'skin.lua').exists())
                data = (folder / 'preview.png').read_bytes()
                self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
                self.assertEqual(struct.unpack('>II', data[16:24]), (240, 136))

    def test_catalog_append_preserves_existing_entries(self):
        current = json.loads((SKINS / 'index.json').read_text(encoding='utf-8'))
        before = json.loads((ROOT / 'backups/DAYNIGHT_20261107_0409/index.json').read_text(encoding='utf-8'))
        for prior in before['skins']:
            if prior['id']!='metro':self.assertIn(prior,current['skins'])
        self.assertEqual({x['id']for x in current['skins'][:len(before['skins'])]},{x['id']for x in before['skins']})
        ids = [item['id'].lower() for item in current['skins']]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn('daylight',ids);self.assertIn('night',ids);self.assertNotIn('metro_day',ids)
        self.assertIn('xmb',ids);self.assertEqual(len(current['skins']), len(before['skins']) + 5)
        for entry in (x for x in current['skins']if x['id']in('daylight','night')):
            self.assertIs(entry['script'], False)
            self.assertIs(entry['preview'], True)

    def test_generator_supports_both_without_changing_script_templates(self):
        source = (ROOT / 'tools/make_skin_examples.py').read_text(encoding='utf-8')
        backup = (ROOT / 'backups/DAYNIGHT_20261107_0409/make_skin_examples.py').read_text(encoding='utf-8')
        def templates(text):
            return {node.targets[0].id: ast.literal_eval(node.value)
                    for node in ast.parse(text).body
                    if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
                    and node.targets[0].id == 'paper'}
        self.assertEqual(templates(source), templates(backup))
        self.assertIn("metro_template=R/'tools/skin_templates/metro.lua'",source)
        self.assertEqual((ROOT/'tools/skin_templates/metro.lua').read_bytes(),(SKINS/'metro/skin.lua').read_bytes())
        self.assertIn("('daylight','白天模式','#C20C0C',False)", source)
        self.assertIn("('night','黑夜模式','#EC4141',False)", source)


if __name__ == '__main__':
    unittest.main()
