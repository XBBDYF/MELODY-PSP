/* UI-thread-only rotating vinyl. No timers or playback ownership here. */
#include "skin_disc.h"
#include "gfx.h"
#include "api.h"
#include "app.h"
#include <pspkernel.h>
#include <pspsysmem.h>
#include <math.h>
#include <stddef.h>

#define DISC_TEX 128
#define DISC_BYTES (DISC_TEX * DISC_TEX * 4)

static SceUID disc_block = -1;
static unsigned char *rgba;
static const unsigned short *cached_src;
static int cached_w, cached_h;
static long long cached_song;
static unsigned int cached_hash;
static int cached_valid;

void skin_disc_release(void)
{
    if (disc_block >= 0)
        sceKernelFreePartitionMemory(disc_block);
    disc_block = -1;
    rgba = NULL;
    cached_src = NULL;
    cached_w = cached_h = cached_valid = 0;
    cached_song = 0;
    cached_hash = 0;
}

static int allocate_texture(void)
{
    if (rgba) return 1;
    disc_block = sceKernelAllocPartitionMemory(2, "skin_disc", PSP_SMEM_High,
                                              DISC_BYTES, NULL);
    if (disc_block < 0) return 0;
    rgba = sceKernelGetBlockHeadAddr(disc_block);
    if (!rgba) {
        skin_disc_release();
        return 0;
    }
    return 1;
}

/* API may reuse its art buffer: song identity alone cannot invalidate it.
 * Sixteen distributed pixels also notice a late cover arriving for that song.
 */
static unsigned int art_hash(const unsigned short *src, int w, int h)
{
    unsigned int hash = 2166136261u;
    int i;
    for (i = 0; i < 16; ++i) {
        int x = ((i % 4) * (w - 1)) / 3;
        int y = ((i / 4) * (h - 1)) / 3;
        hash = (hash ^ src[(size_t)y * w + x]) * 16777619u;
    }
    return hash;
}

static void snapshot(const unsigned short *src, int w, int h)
{
    int crop = w < h ? w : h;
    int left = (w - crop) / 2, top = (h - crop) / 2;
    int x, y;
    for (y = 0; y < DISC_TEX; ++y) {
        int sy = top + ((2 * y + 1) * crop) / (2 * DISC_TEX);
        int dy = 2 * y - (DISC_TEX - 1);
        for (x = 0; x < DISC_TEX; ++x) {
            int dx = 2 * x - (DISC_TEX - 1);
            unsigned char *p = rgba + (y * DISC_TEX + x) * 4;
            if (dx * dx + dy * dy >= DISC_TEX * DISC_TEX) {
                p[0] = p[1] = p[2] = p[3] = 0;
            } else {
                int sx = left + ((2 * x + 1) * crop) / (2 * DISC_TEX);
                unsigned short color = src[(size_t)sy * w + sx];
                int r = color & 31, g = (color >> 5) & 63;
                int b = (color >> 11) & 31;
                p[0] = (unsigned char)((r << 3) | (r >> 2));
                p[1] = (unsigned char)((g << 2) | (g >> 4));
                p[2] = (unsigned char)((b << 3) | (b >> 2));
                p[3] = 255;
            }
        }
    }
}

int skin_disc_draw(int x, int y, int size, float angle)
{
    int w = 0, h = 0, available = 0;
    int cx, cy, radius, cover_size, offset;
    const unsigned short *src;
    unsigned int hash;
    /* Parent admits bounds; keep direct callers from malformed geometry. */
    if (size < 32 || size > 224) return 0;
    cx = x + size / 2;
    cy = y + size / 2;
    radius = (size - 1) / 2;
    gfx_circle(cx, cy, radius, gfx_rgb(22, 23, 26));
    gfx_ring(cx, cy, radius - 2, radius - 3, gfx_rgb(48, 49, 53));
    gfx_ring(cx, cy, radius * 4 / 5, radius * 4 / 5 - 1,
             gfx_rgb(35, 36, 40));
    cover_size = size * 3 / 5;
    offset = (size - cover_size) / 2;
    gfx_circle(cx, cy, cover_size / 2, gfx_rgb(68, 69, 75));

    src = api_art(&w, &h);
    if (src && w > 0 && h > 0 && w <= 4096 && h <= 4096 && allocate_texture()) {
        hash = art_hash(src, w, h);
        if (!cached_valid || cached_src != src || cached_w != w || cached_h != h ||
            cached_song != g_app.now.id || cached_hash != hash) {
            snapshot(src, w, h);
            cached_src = src;
            cached_w = w;
            cached_h = h;
            cached_song = g_app.now.id;
            cached_hash = hash;
            cached_valid = 1;
        }
        if (!isfinite(angle)) angle = 0.0f;
        angle = fmodf(angle, 360.0f);
        if (angle < 0.0f) angle += 360.0f;
        gfx_blit_rgba_rotated(x + offset, y + offset, cover_size,
                              rgba, DISC_TEX, DISC_TEX, angle);
        available = 1;
    }
    gfx_circle(cx, cy, size / 28 + 1, gfx_rgb(16, 17, 19));
    gfx_circle(cx, cy, size / 80 + 1, gfx_rgb(191, 192, 195));
    return available;
}
