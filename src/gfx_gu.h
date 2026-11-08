#ifndef MELODY_GFX_GU_H
#define MELODY_GFX_GU_H

/* Main/UI thread only. Parent owns framebuffer allocation, display and swaps.
 * Colors use PSP RGB565 (red in low five bits), alpha is 0..255.
 * Clip bounds are exclusive. Source stride is in pixels; RGBA bytes are R,G,B,A.
 * Image/glyph identities reuse their first snapshot until flush; keep a
 * source pointer's contents stable during that interval, or flush on mutation.
 * Images accept source dimensions 1..512; unsupported input returns 0 before
 * issuing draws so the caller can flush, draw on CPU, then resume.
 * Glyph rows are packed MSB first with (w+7)/8 bytes per row, up to 64x64.
 * init lazily allocates 768 KiB in partition 2/high; shutdown frees all of it.
 */
enum { GFX_GU_5650 = 0, GFX_GU_8888 = 3 };
typedef struct GfxGuStats {
    unsigned draws, uploads, flushes;
} GfxGuStats;
int gfx_gu_init(void);
void gfx_gu_shutdown(void);
/* draw_vram_offset is 0 or 0x44000, never an absolute VRAM pointer. */
int gfx_gu_begin(void *draw_vram_offset);
void gfx_gu_end(void);
/* flush waits for GE completion and closes the list; resume reopens it. */
void gfx_gu_flush(void);
void gfx_gu_resume(void);
void gfx_gu_clip(int x0, int y0, int x1, int y1);
void gfx_gu_rect(int x, int y, int w, int h, unsigned short color, int alpha);
int gfx_gu_image(int x, int y, int dw, int dh, const void *src,
                 int sw, int sh, int stride, int format, int alpha);
/* Quad corners and UVs are TL, TR, BL, BR; UV units are source pixels.
 * Source identity is cached until a list flush: callers must keep a given
 * source pointer's contents stable during that interval. Tint is RGB565. */
int gfx_gu_quad565(const unsigned short *src, int sw, int sh, int stride,
                    const float xy[8], const float uv[8], unsigned short tint);
int gfx_gu_quad8888_alpha(const unsigned char*src,int sw,int sh,const float xy[8],const float uv[8],int alpha);
int gfx_gu_quad8888(const unsigned char*src,int sw,int sh,const float xy[8],const float uv[8]);
int gfx_gu_glyph(int x, int y, int w, int h, const unsigned char *bits,
                 unsigned short color, int alpha);
/* Counters reset at begin, remain readable after end/shutdown. */
int gfx_gu_readback565(const void*src,void*dst);
void gfx_gu_get_stats(GfxGuStats *out);
#endif
