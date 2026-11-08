/* Fixed-size album thumbnail conversion; no heap, PSP APIs, or codec state. */
#ifndef MELODY_ART_FIT_H
#define MELODY_ART_FIT_H
#include <stddef.h>

#define ART_FIT_MAX_DIM 192

/* Aspect-fit tightly packed RGBA onto a PSP RGB565 canvas, centered with
 * black padding. Nearest-neighbor scaling permits enlargement, preserves the
 * full cover, and keeps the fixed cache stride. Alpha is ignored, matching the
 * existing rgba_to_565 conversion (red low five bits, blue high five bits).
 * All dimensions are 1..192. Capacities are bytes, buffers must not overlap.
 * Returns 0 on success; -1 for invalid dimensions/pointers/capacities, without
 * modifying the destination. Uses only constant-size local state.
 */
static inline int art_fit_rgba565(const unsigned char *rgba, size_t rgba_bytes,
                                  int sw, int sh, unsigned short *out,
                                  size_t out_bytes, int dw, int dh)
{
    int x, y, fw, fh, ox, oy;
    size_t need_src, need_dst;
    if (!rgba || !out || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        sw > ART_FIT_MAX_DIM || sh > ART_FIT_MAX_DIM ||
        dw > ART_FIT_MAX_DIM || dh > ART_FIT_MAX_DIM)
        return -1;
    /* Bounds above keep these products safe on the PSP's 32-bit size_t. */
    need_src = (size_t)sw * (size_t)sh * 4u;
    need_dst = (size_t)dw * (size_t)dh * 2u;
    if (rgba_bytes < need_src || out_bytes < need_dst) return -1;
    if (sw * dh >= sh * dw) {
        fw = dw;
        fh = sh * dw / sw;
    } else {
        fh = dh;
        fw = sw * dh / sh;
    }
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    ox = (dw - fw) / 2;
    oy = (dh - fh) / 2;
    for (y = 0; y < dh; ++y)
        for (x = 0; x < dw; ++x)
            out[(size_t)y * (size_t)dw + (size_t)x] = 0;
    for (y = 0; y < fh; ++y) {
        int sy = y * sh / fh;
        const unsigned char *row = rgba + (size_t)sy * (size_t)sw * 4u;
        for (x = 0; x < fw; ++x) {
            int sx = x * sw / fw;
            const unsigned char *p = row + (size_t)sx * 4u;
            out[(size_t)(y + oy) * (size_t)dw + (size_t)(x + ox)] =
                (unsigned short)(((p[2] >> 3) << 11) |
                                 ((p[1] >> 2) << 5) | (p[0] >> 3));
        }
    }
    return 0;
}
#endif
