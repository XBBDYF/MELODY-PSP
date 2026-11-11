#ifndef MELODY_ULTRA_PLAYER_H
#define MELODY_ULTRA_PLAYER_H

/* Main thread only. Parent isolates its main loop, disables networking and
 * handles theme exit. No Lua, scanning, downloading, cover art or animations.
 * init is safe before audio initialization; enter stops the previous stream.
 * enter opens the local list without autoplay. draw never flips the display.
 * tick returns 1 only when Circle confirms the sole settings item (exit).
 * pressed is the button edge mask; held/dt are reserved (no repeat polling).
 * CPU 133 MHz is a suggested starting point, requiring hardware audio tests. */
void ultra_player_init(void);
void ultra_player_enter(void);
int ultra_player_tick(unsigned int pressed, unsigned int held, float dt);
void ultra_player_draw(void);
int ultra_player_needs_ui_clock(void);

#endif
