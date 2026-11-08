"""Status ownership regression: player's old clock must not be overdrawn by common HUD."""
from pathlib import Path
R=Path(__file__).resolve().parents[1]
s=(R/'src/ui.c').read_text(encoding='utf-8');a=s.index('static void screen_player(');b=s.index('\nstatic ',a+10);p=s[a:b]
assert 'draw_clock('not in p
assert 'gfx_text_right(FS_SM, 268, 8, right'in p
n=s[s.index('static void screen_net_speed('):s.index('static void export_progress_draw(')]
assert 'store_show_speed()'in n and 'nav_status_draw()'in n and 'fill_rect'not in n
assert 'gfx_text_right(FS_SM,350,8' in n or 'gfx_text_right(FS_SM,352,8' in n
status=s[s.index('static void nav_status_draw('):s.index('static void nav_tabs(')]
assert '402, 8, status.clock'in status and '432, 10'in status
print('PASS player bitrate <=268, speed <=350, clock <=402, battery432+: one clock owner and preserved speed toggle')
