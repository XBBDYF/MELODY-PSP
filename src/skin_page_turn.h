#ifndef SKIN_PAGE_TURN_H
#define SKIN_PAGE_TURN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Capture the just-rendered backbuffer (480 x 272 RGB565, stride 512).
 * One independent high-partition slot; returns 1 on success, 0 on failure.
 * Call at the end of rendering the old page, before rendering the new page.
 */
int skin_page_capture(void);

/* Fold one captured source rectangle around the COMMON PAGE LEFT spine
 * (screen x=0, Y axis) onto the CURRENT backbuffer. All planes share this axis;
 * projected pixels may move left of the source rectangle. Its right edge
 * narrows vertically with depth around its own vertical center.
 * Source rectangle is intersected with the screen; target respects gfx clip.
 * progress <= 0 reproduces old pixels exactly; progress >= 1 draws nothing.
 * No clearing or allocation. Caller owns per-plane delays and waiting/ready.
 * NaN selects progress 0; positive infinity selects progress 1.
 */
void skin_page_fold(float progress, int x, int y, int w, int h);

/* Compatibility wrapper: fold rectangle (0, top, 480, bottom-top).
 * top/bottom are clamped to [0, 272]; background is ignored.
 */
void skin_page_turn(float progress, int top, int bottom,
                    unsigned short background);

/* Release on reset, MV entry or skin switch; safe to call repeatedly. */
void skin_page_release(void);
int skin_page_active(void);

#ifdef __cplusplus
}
#endif
#endif
