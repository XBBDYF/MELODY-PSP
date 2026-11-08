"""Modal overlays never initiate Metro turns; navigation/category turns remain."""
from pathlib import Path
R=Path(__file__).resolve().parents[1];ui=(R/'src/ui.c').read_text(encoding='utf-8');main=(R/'src/main.c').read_text(encoding='utf-8')
draw=ui[ui.index('void ui_draw(void)'):];assert 'modal_signature'not in draw and 'skin_transition_request_section'not in draw
for fn in ['confirm_draw();','player_menu_draw();','skin_options_dialog_draw();','upd_dialog_draw();']:assert fn in draw
assert 'skin_transition_request(old, sc, g_app.time)'in main or 'skin_transition_request(old,sc,g_app.time)'in main or 'skin_transition_request('in main
assert 'skin_transition_wait_section('in main and 'skin_transition_ready_section('in main
assert 'previous_document'not in main
print('PASS immediate function dialogs; page and async category navigation retain turns')
