"""Geometry contract prevents XMB background regressing to a single thin line."""
from pathlib import Path
import re,unittest
R=Path(__file__).resolve().parents[1]
class WaveFidelity(unittest.TestCase):
 def test_wave_is_broad_layered_and_optional(self):
  c=(R/'src/skin_wave.c').read_text(encoding='utf-8');lua=(R/'skin_release/melody/skins/xmb/skin.lua').read_text(encoding='utf-8')
  self.assertIn('for(int layer=2;layer>=0;--layer)',c);self.assertIn('for(int i=0;i<30;i++)',c);self.assertIn('skin_wave_draw',c);self.assertGreaterEqual(c.count('float a='),1);self.assertGreaterEqual(c.count('float b='),1)
  self.assertIn('if ui.wave then ui.wave(',lua);self.assertIn('p.wave',lua);self.assertIn('p.asset',lua);self.assertNotIn('for strand=0,0 do for i=0,7 do',lua)
if __name__=='__main__':unittest.main()
