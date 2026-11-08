#include "gfx_gu.h"
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspgu.h>
#include <stdint.h>
#include <string.h>

#define LIST_BYTES (256u * 1024u)
#define ARENA_BYTES (512u * 1024u)
#define COMMAND_LIMIT (200u * 1024u)

typedef struct { float u, v, x, y, z; } TexVertex;
typedef struct { float x, y, z; } FlatVertex;
static SceUID pool_id = -1;
static unsigned char *list, *arena;
static unsigned arena_used, command_used;
static int initialized, frame, active;
static void *draw_offset;
static int cx0, cy0, cx1 = 480, cy1 = 272;
static GfxGuStats stats;
typedef struct {
    const void *src;
    int sw, sh, stride, format, sy, rows, tw, th;
    unsigned char *pixels;
} ImageCache;
static ImageCache images[8];
static unsigned image_next;
typedef struct {
    const unsigned char *bits;
    int w, h;
    unsigned short color;
    unsigned *pixels;
} GlyphCache;
static GlyphCache glyphs[64];
static unsigned glyph_next;
static void reserve(unsigned bytes, unsigned commands);

static int clamp(int x, int hi) { return x < 0 ? 0 : (x > hi ? hi : x); }
static int power2(int n) { int p = 1; while (p < n) p <<= 1; return p; }
static unsigned rgba565(unsigned short c, int alpha)
{
    unsigned r = c & 31, g = (c >> 5) & 63, b = (c >> 11) & 31;
    return (r * 255 / 31) | ((g * 255 / 63) << 8) |
           ((b * 255 / 31) << 16) | ((unsigned)clamp(alpha, 255) << 24);
}

int gfx_gu_init(void)
{
    void *head;
    if (initialized) return 1;
    pool_id = sceKernelAllocPartitionMemory(2, "melody_gu", PSP_SMEM_High,
                                            LIST_BYTES + ARENA_BYTES, 0);
    if (pool_id < 0) return 0;
    head = sceKernelGetBlockHeadAddr(pool_id);
    if (!head || ((uintptr_t)head & 63)) {
        sceKernelFreePartitionMemory(pool_id);
        pool_id = -1;
        return 0;
    }
    list = head;
    arena = list + LIST_BYTES;
    sceGuInit();
    initialized = 1;
    return 1;
}

void gfx_gu_resume(void)
{
    if (!initialized || !frame || active) return;
    arena_used = command_used = 0;
    memset(images, 0, sizeof(images));
    image_next = 0;
    memset(glyphs, 0, sizeof(glyphs));
    glyph_next = 0;
    sceGuStart(GU_DIRECT, list);
    active = 1;
    sceGuDrawBuffer(GU_PSM_5650, draw_offset, 512);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuScissor(cx0, cy0, cx1, cy1);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_LIGHTING);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    command_used = 1024;
}

void gfx_gu_flush(void)
{
    if (!active) return;
    sceGuFinish();
    sceGuSync(0, 0);
    active = 0;
    ++stats.flushes;
}

int gfx_gu_begin(void *offset)
{
    if ((uintptr_t)offset != 0 && (uintptr_t)offset != 0x44000) return 0;
    if (!gfx_gu_init()) return 0;
    gfx_gu_flush();
    memset(&stats, 0, sizeof(stats));
    draw_offset = offset;
    frame = 1;
    gfx_gu_resume();
    return 1;
}

void gfx_gu_end(void)
{
    gfx_gu_flush();
    frame = 0;
}

void gfx_gu_shutdown(void)
{
    gfx_gu_end();
    if (initialized) sceGuTerm();
    initialized = 0;
    if (pool_id >= 0) sceKernelFreePartitionMemory(pool_id);
    pool_id = -1;
    list = arena = 0;
    arena_used = command_used = 0;
}

int gfx_gu_readback565(const void*src,void*dst){
 if(!initialized||!src||!dst)return 0;
 gfx_gu_flush();sceKernelDcacheWritebackInvalidateRange(dst,512*272*2);
 sceGuStart(GU_DIRECT,list);sceGuCopyImage(GU_PSM_5650,0,0,480,272,512,src,0,0,512,dst);sceGuFinish();sceGuSync(0,0);
 sceKernelDcacheInvalidateRange(dst,512*272*2);return 1;
}

void gfx_gu_get_stats(GfxGuStats *out) { if (out) *out = stats; }

void gfx_gu_clip(int x0, int y0, int x1, int y1)
{
    cx0 = clamp(x0, 480); cy0 = clamp(y0, 272);
    cx1 = clamp(x1, 480); cy1 = clamp(y1, 272);
    if (cx1 < cx0) cx1 = cx0;
    if (cy1 < cy0) cy1 = cy0;
    if (active) { reserve(0, 32); sceGuScissor(cx0, cy0, cx1, cy1); }
}

/* Reserve conservatively before any texture or vertex referenced by a batch.
 * Reuse is permitted only after the previous list has finished on the GE. */
static void reserve(unsigned bytes, unsigned commands)
{
    if (arena_used + bytes > ARENA_BYTES || command_used + commands > COMMAND_LIMIT) {
        gfx_gu_flush();
        gfx_gu_resume();
    }
    command_used += commands;
}

void gfx_gu_rect(int x, int y, int w, int h, unsigned short color, int alpha)
{
    FlatVertex *v;
    if (!active || w <= 0 || h <= 0 || alpha <= 0 || cx0 == cx1 || cy0 == cy1) return;
    if(x<cx0){w-=cx0-x;x=cx0;}if(y<cy0){h-=cy0-y;y=cy0;}
    if(x+w>cx1)w=cx1-x;
    if(y+h>cy1)h=cy1-y;
    if(w<=0||h<=0)return;
    reserve(0, 128);
    v = sceGuGetMemory(2 * sizeof(*v));
    v[0].x = (float)x; v[0].y = (float)y; v[0].z = 0;
    v[1].x = (float)x + w; v[1].y = (float)y + h; v[1].z = 0;
    sceKernelDcacheWritebackRange(v, 2 * sizeof(*v));
    sceGuDisable(GU_TEXTURE_2D);
    sceGuColor(rgba565(color, alpha));
    sceGuDrawArray(GU_SPRITES, GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    ++stats.draws;
}

static unsigned char *upload_space(int tw, int th, int bytes, unsigned commands)
{
    unsigned size = (unsigned)tw * th * bytes;
    unsigned char *p;
    reserve(size, commands);
    p = arena + arena_used;
    arena_used += (size + 63) & ~63u;
    memset(p, 0, size);
    ++stats.uploads;
    return p;
}

static void texture(int format, int tw, int th, void *p, int alpha)
{
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(format, 0, 0, 0);
    sceGuTexImage(0, tw, th, tw, p);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFlush();
    sceGuColor(0xffffffu | ((unsigned)clamp(alpha, 255) << 24));
}

static unsigned char *image_snapshot(const void *src, int sw, int sh, int stride,
                                      int format, int sy, int rows, unsigned commands)
{
    int i, row, tw = power2(sw), th = power2(rows);
    int bytes = format == GFX_GU_5650 ? 2 : 4;
    unsigned char *p;
    ImageCache *entry;
    reserve(0, commands);
    for (i = 0; i < 8; ++i) {
        entry = &images[i];
        if (entry->src == src && entry->sw == sw && entry->sh == sh &&
            entry->stride == stride && entry->format == format &&
            entry->sy == sy && entry->rows == rows) return entry->pixels;
    }
    p = upload_space(tw, th, bytes, commands);
    for (row = 0; row < rows; ++row)
        memcpy(p + (size_t)row * tw * bytes,
               (const unsigned char *)src + (size_t)(sy + row) * stride * bytes,
               (size_t)sw * bytes);
    sceKernelDcacheWritebackRange(p, (unsigned)tw * th * bytes);
    entry = &images[image_next++ % 8];
    entry->src = src; entry->sw = sw; entry->sh = sh; entry->stride = stride;
    entry->format = format; entry->sy = sy; entry->rows = rows;
    entry->tw = tw; entry->th = th; entry->pixels = p;
    return p;
}

static void sprite(float x0, float y0, float x1, float y1,
                   float u0, float v0, float u1, float v1)
{
    /* Reject/trim before GE fixed-point conversion: negative scrolling
     * coordinates must never wrap through the status/header region. */
    if(x1<=cx0||x0>=cx1||y1<=cy0||y0>=cy1||x1<=x0||y1<=y0)return;
    float du=(u1-u0)/(x1-x0),dv=(v1-v0)/(y1-y0);
    if(x0<cx0){u0+=(cx0-x0)*du;x0=(float)cx0;}
    if(y0<cy0){v0+=(cy0-y0)*dv;y0=(float)cy0;}
    if(x1>cx1){u1-=(x1-cx1)*du;x1=(float)cx1;}
    if(y1>cy1){v1-=(y1-cy1)*dv;y1=(float)cy1;}
    TexVertex *v = sceGuGetMemory(2 * sizeof(*v));
    v[0].u = u0; v[0].v = v0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
    v[1].u = u1; v[1].v = v1; v[1].x = x1; v[1].y = y1; v[1].z = 0;
    sceKernelDcacheWritebackRange(v, 2 * sizeof(*v));
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    ++stats.draws;
}

int gfx_gu_image(int x, int y, int dw, int dh, const void *src,
                 int sw, int sh, int stride, int format, int alpha)
{
    int tw, maxh, bytes, sy;
    if (!active || !src || sw <= 0 || sh <= 0 || sw > 512 || sh > 512 ||
        stride < sw || stride > 1048576 || dw <= 0 || dh <= 0 || dw > 4096 || dh > 4096 ||
        (format != GFX_GU_5650 && format != GFX_GU_8888)) return 0;
    if (alpha <= 0 || cx0 == cx1 || cy0 == cy1) return 1;
    bytes = format == GFX_GU_5650 ? 2 : 4;
    tw = power2(sw);
    maxh = (int)(ARENA_BYTES / (tw * bytes));
    if (maxh > 512) maxh = 512;
    for (sy = 0; sy < sh; sy += maxh) {
        int rows = sh - sy, th, dx;
        unsigned char *p;
        if (rows > maxh) rows = maxh;
        th = power2(rows);
        p = image_snapshot(src, sw, sh, stride, format, sy, rows,
                           (unsigned)((dw + 63) / 64) * 160 + 512);
        texture(format, tw, th, p, alpha);
        for (dx = 0; dx < dw; dx += 64) {
            int end = dx + 64 < dw ? dx + 64 : dw;
            sprite((float)x + dx, (float)y + (float)sy * dh / sh,
                   (float)x + end, (float)y + (float)(sy + rows) * dh / sh,
                   (float)dx * sw / dw, 0, (float)end * sw / dw, (float)rows);
        }
    }
    return 1;
}

int gfx_gu_quad565(const unsigned short *src, int sw, int sh, int stride,
                    const float xy[8], const float uv[8], unsigned short tint)
{
    int i;
    unsigned char *p;
    TexVertex *v;
    if (!active || !src || !xy || !uv || sw <= 0 || sh <= 0 || sw > 512 || sh > 512 ||
        stride < sw || stride > 1048576) return 0;
    for (i = 0; i < 8; ++i)
        if (!(xy[i] >= -32768 && xy[i] <= 32767 && uv[i] >= 0 &&
              uv[i] <= (i % 2 ? sh : sw))) return 0;
    if (cx0 == cx1 || cy0 == cy1) return 1;
    p = image_snapshot(src, sw, sh, stride, GFX_GU_5650, 0, sh, 640);
    texture(GU_PSM_5650, power2(sw), power2(sh), p, 255);
    sceGuColor(rgba565(tint, 255));
    v = sceGuGetMemory(4 * sizeof(*v));
    for (i = 0; i < 4; ++i) {
        v[i].x = xy[i * 2]; v[i].y = xy[i * 2 + 1]; v[i].z = 0;
        v[i].u = uv[i * 2]; v[i].v = uv[i * 2 + 1];
    }
    sceKernelDcacheWritebackRange(v, 4 * sizeof(*v));
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   4, 0, v);
    ++stats.draws;
    return 1;
}

int gfx_gu_quad8888_alpha(const unsigned char*src,int sw,int sh,const float xy[8],const float uv[8],int alpha)
{
 if(!active||!src||!xy||!uv||sw<=0||sh<=0||sw>256||sh>256)return 0;
 if(alpha<=0||cx0==cx1||cy0==cy1)return 1;
 for(int i=0;i<8;++i)if(!(xy[i]>=-2048&&xy[i]<=2048&&uv[i]>=0&&uv[i]<=(i%2?sh:sw)))return 0;
 unsigned char*p=image_snapshot(src,sw,sh,sw,GFX_GU_8888,0,sh,640);
 texture(GU_PSM_8888,power2(sw),power2(sh),p,alpha);
 TexVertex*v=sceGuGetMemory(4*sizeof(*v));
 for(int i=0;i<4;++i){v[i].u=uv[i*2];v[i].v=uv[i*2+1];v[i].x=xy[i*2];v[i].y=xy[i*2+1];v[i].z=0;}
 sceKernelDcacheWritebackRange(v,4*sizeof(*v));sceGuDrawArray(GU_TRIANGLE_STRIP,GU_TEXTURE_32BITF|GU_VERTEX_32BITF|GU_TRANSFORM_2D,4,0,v);stats.draws++;return 1;
}

int gfx_gu_quad8888(const unsigned char*src,int sw,int sh,const float xy[8],const float uv[8]){return gfx_gu_quad8888_alpha(src,sw,sh,xy,uv,255);}

int gfx_gu_glyph(int x, int y, int w, int h, const unsigned char *bits,
                 unsigned short color, int alpha)
{
    int tw, th, xx, yy, stride, i;
    unsigned *p = 0, c;
    GlyphCache *entry;
    if (!active || !bits || w <= 0 || h <= 0 || w > 64 || h > 64) return 0;
    if (alpha <= 0 || cx0 == cx1 || cy0 == cy1) return 1;
    tw = power2(w); th = power2(h); stride = (w + 7) / 8;
    reserve(0, 512);
    for (i = 0; i < 64; ++i)
        if (glyphs[i].bits == bits && glyphs[i].w == w && glyphs[i].h == h &&
            glyphs[i].color == color) { p = glyphs[i].pixels; break; }
    if (!p) {
        p = (unsigned *)upload_space(tw, th, 4, 512);
        c = rgba565(color, 255);
        for (yy = 0; yy < h; ++yy)
            for (xx = 0; xx < w; ++xx)
                if (bits[yy * stride + xx / 8] & (0x80 >> (xx & 7))) p[yy * tw + xx] = c;
        sceKernelDcacheWritebackRange(p, (unsigned)tw * th * 4);
        entry = &glyphs[glyph_next++ % 64];
        entry->bits = bits; entry->w = w; entry->h = h;
        entry->color = color; entry->pixels = p;
    }
    texture(GU_PSM_8888, tw, th, p, alpha);
    sprite((float)x, (float)y, (float)x + w, (float)y + h, 0, 0, (float)w, (float)h);
    return 1;
}
