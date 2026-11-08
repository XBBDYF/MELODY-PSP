"""Visible shortcuts must match production player/list/search input."""
from pathlib import Path
import re
R=Path(__file__).resolve().parents[1];ui=(R/'src/ui.c').read_text(encoding='utf-8');main=(R/'src/main.c').read_text(encoding='utf-8');runtime=(R/'src/skin_runtime.c').read_text(encoding='utf-8')
p=ui[ui.index('static void screen_player('):ui.index('static void screen_lyrics(')];assert 'hint_item("△","歌词")'in p and 'hint_item("SELECT","菜单")'in p;assert p.index('hint_item("△"')<p.index('hint_item("○"')
assert 'field_i(L,"download_select_mode",g_app.download_select_mode)'in runtime
for name in ['metro','metro_day']:
 s=(R/'skin_release/melody/skins'/name/'skin.lua').read_text(encoding='utf-8');assert 'SELECT菜单'in s and 'L多选下载'in s and '□选/取消'in s and 'SELECT多选下载'in s
assert 'PSP_CTRL_LTRIGGER){g_app.download_select_mode='in main
assert 'if(pressed&PSP_CTRL_SELECT){g_app.download_select_mode='in main
print('PASS default Triangle lyrics/Select menu prioritised, both Metro modes show Select, actual list L/search Select multi-download hints')
