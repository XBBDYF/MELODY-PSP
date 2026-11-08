"""Regression guards for native overlays surviving custom skin pages."""
from pathlib import Path
import re
import unittest

UI = (Path(__file__).resolve().parents[1] / 'src' / 'ui.c').read_text(encoding='utf-8-sig')

def body(name):
    match = re.search(r'\b' + name + r'\([^;]*?\)\s*\{', UI)
    if not match:
        raise AssertionError(f'missing function {name}')
    start = match.end()
    depth = 1
    for end in range(start, len(UI)):
        depth += (UI[end] == '{') - (UI[end] == '}')
        if depth == 0:
            return UI[start:end]
    raise AssertionError(f'unclosed function {name}')

class SkinModalTests(unittest.TestCase):
    def test_player_menu_survives_custom_draw_without_double_render(self):
        self.assertNotIn('player_menu_draw()', body('screen_player'))
        draw = body('ui_draw')
        self.assertGreater(draw.index('player_menu_draw()'), draw.index('skin_runtime_draw('))
        self.assertIn('g_app.screen == SC_PLAYER && g_app.menu_open', draw)

    def test_metro_family_uses_active_style_and_native_wifi(self):
        self.assertIn('skin_runtime_active()', body('is_metro_style'))
        self.assertIn('!strcmp(id,"metro")',body('is_metro_style'));self.assertIn('!strcmp(id,"metro_day")',body('is_metro_style'))
        self.assertIn('screen_wifi_metro', body('screen_wifi'))
        wifi = body('screen_wifi_metro')
        for state in ('g_app.wlan_on', 'g_app.profile_count', 'g_app.profile_sel', 'g_app.connecting', 'g_app.connect_msg'):
            self.assertIn(state, wifi)
        self.assertNotIn('gfx_fill_round', wifi)

    def test_flat_modal_helpers_cover_all_existing_native_dialogs(self):
        for function in ('confirm_draw', 'upd_dialog_draw', 'player_menu_draw', 'toast_draw', 'export_progress_draw'):
            self.assertIn('modal_fill', body(function), function)
        self.assertIn('is_metro_style()', body('ui_document'))
        self.assertIn('return C_BG',body('metro_background'))
        self.assertIn('gfx_fill_rect', body('modal_fill'))

if __name__ == '__main__':
    unittest.main()
