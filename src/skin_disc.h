#ifndef MELODY_SKIN_DISC_H
#define MELODY_SKIN_DISC_H

/* Main/UI thread only. Parent validates size 32..224 and screen bounds.
 * angle is clockwise degrees (normalized internally). Always draws vinyl;
 * returns 1 when a circular cover texture is available, otherwise 0.
 * Parent owns animation timing, clipping, and calls release on MV/hot switch.
 */
int skin_disc_draw(int x, int y, int size, float angle);
void skin_disc_release(void);

#endif
