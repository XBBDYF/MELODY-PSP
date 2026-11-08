#include "skin_page_turn.h"
#include "gfx.h"
#include <pspkernel.h>
#include <pspsysmem.h>
#include <math.h>
#include <string.h>

/* Packed visible pixels: 261120 bytes, plus one UID and one pointer.
 * No per-frame allocations and no access to scanout/VRAM. */
#define PAGE_BYTES (SCR_W * SCR_H * sizeof(unsigned short))
static SceUID page_block = -1;
static unsigned short *page_pixels;

int skin_page_active(void)
{
    return page_pixels != 0;
}

void skin_page_release(void)
{
    page_pixels = 0;
    if (page_block >= 0) sceKernelFreePartitionMemory(page_block);
    page_block = -1;
}

int skin_page_capture(void)
{
    unsigned short *fb = gfx_backbuffer();
    int y;
    if (!fb) return 0;
    if (!page_pixels) {
        SceUID block = sceKernelAllocPartitionMemory(2, "skin_page", PSP_SMEM_High,
                                                     PAGE_BYTES, 0);
        unsigned short *pixels;
        if (block < 0) return 0;
        pixels = (unsigned short *)sceKernelGetBlockHeadAddr(block);
        if (!pixels) {
            sceKernelFreePartitionMemory(block);
            return 0;
        }
        page_block = block;
        page_pixels = pixels;
    }
    for (y = 0; y < SCR_H; ++y)
        memcpy(page_pixels + y * SCR_W, fb + y * FB_STRIDE,
               SCR_W * sizeof(unsigned short));
    return 1;
}

static unsigned short shade565(unsigned short c, int light)
{
    return (unsigned short)((((c & 31) * light >> 8)) |
           ((((c >> 5) & 63) * light >> 8) << 5) |
           ((((c >> 11) & 31) * light >> 8) << 11));
}

void skin_page_fold(float progress, int rx, int ry, int width, int height)
{
    unsigned short *fb;
    long long right, bottom;
    int x0, y0, x1, y1, x, y, light;
    float sine, cosine, projected_left, projected_right, center_y;
    const float distance = 420.0f;
    if (!page_pixels || width <= 0 || height <= 0) return;
    /* NaN and negative progress select the exact closed plane; +inf is open. */
    if (!(progress >= 0.0f)) progress = 0.0f;
    if (progress >= 1.0f) return;
    /* Widen additions before clipping, including extreme caller coordinates. */
    right = (long long)rx + width;
    bottom = (long long)ry + height;
    if (rx >= SCR_W || ry >= SCR_H || right <= 0 || bottom <= 0) return;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (right > SCR_W) right = SCR_W;
    if (bottom > SCR_H) bottom = SCR_H;
    height = (int)bottom - ry;
    /* GE handles texture projection; keep source pixels in the reusable RAM
     * snapshot and approximate perspective with narrow source-space strips. */
    if(gfx_renderer()==GFX_RENDERER_GE){
        float angle=progress*1.5707963267948966f,si=sinf(angle),co=cosf(angle),cy=ry+height*.5f;
        int shade=256-(int)(80*si);unsigned short tint=(unsigned short)(((31*shade>>8))|((63*shade>>8)<<5)|((31*shade>>8)<<11));
        int complete=1;
        for(int left=rx;left<(int)right;left+=24){int end=left+24;if(end>(int)right)end=(int)right;
            float scale0=420.f/(420.f+left*si),scale1=420.f/(420.f+end*si);
            float xy[8]={left*co*scale0,cy-height*.5f*scale0,end*co*scale1,cy-height*.5f*scale1,left*co*scale0,cy+height*.5f*scale0,end*co*scale1,cy+height*.5f*scale1};
            float uv[8]={(float)left,(float)ry,(float)end,(float)ry,(float)left,(float)bottom,(float)end,(float)bottom};
            if(!gfx_ge_page_quad(page_pixels,SCR_W,SCR_H,SCR_W,xy,uv,tint)){complete=0;break;}
        }
        if(complete)return;
    }
    fb = gfx_backbuffer();
    if (!fb) return;
    gfx_get_clip(&x0, &y0, &x1, &y1);
    if (x0 < 0) x0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y0 < ry) y0 = ry;
    if (y1 > (int)bottom) y1 = (int)bottom;
    if (x0 >= x1 || y0 >= y1) return;
    if (progress == 0.0f) {
        if (x0 < rx) x0 = rx;
        if (x1 > (int)right) x1 = (int)right;
        if (x0 >= x1) return;
        for (y = y0; y < y1; ++y)
            memcpy(fb + y * FB_STRIDE + x0, page_pixels + y * SCR_W + x0,
                   (x1 - x0) * sizeof(unsigned short));
        return;
    }
    sine = sinf(progress * 1.5707963267948966f);
    cosine = cosf(progress * 1.5707963267948966f);
    if (cosine <= 0.0f) return;
    /* Every plane shares the page's x=0 spine, not its own rectangle edge. */
    projected_left = rx * cosine * distance / (distance + rx * sine);
    projected_right = (float)right * cosine * distance / (distance + (float)right * sine);
    if (x0 < (int)ceilf(projected_left)) x0 = (int)ceilf(projected_left);
    if (x1 > (int)ceilf(projected_right)) x1 = (int)ceilf(projected_right);
    if (x0 >= x1) return;
    center_y = ry + height * 0.5f;
    light = 256 - (int)(80.0f * sine);
    for (x = x0; x < x1; ++x) {
        float dx = (float)x;
        float denom, source_x, scale, upper, lower;
        int sx, start, end, step, source_y;
        int inner_start, inner_end, far_edge;
        unsigned short *dest;
        if (dx >= projected_right) break;
        /* Global inverse: x = sourceX*cos(a)*D/(D+sourceX*sin(a)). */
        denom = distance * cosine - dx * sine;
        if (denom <= 0.0f) break;
        source_x = dx * distance / denom;
        sx = (int)source_x;
        if (sx < rx || sx >= (int)right) continue;
        scale = distance / (distance + source_x * sine);
        upper = center_y - height * 0.5f * scale;
        lower = center_y + height * 0.5f * scale;
        start = (int)ceilf(upper);
        end = (int)ceilf(lower);
        if (start < y0) start = y0;
        if (end > y1) end = y1;
        if (start >= end) continue;
        inner_start = (int)ceilf(upper + 1.0f);
        inner_end = (int)ceilf(lower - 1.0f);
        far_edge = projected_right - dx <= 1.0f || dx - projected_left < 1.0f;
        /* 16.16 inverse y mapping; no division/float inside pixel loop. */
        step = (int)(65536.0f / scale);
        source_y = (int)(((float)start - upper) * 65536.0f / scale);
        dest = fb + start * FB_STRIDE + x;
        for (y = start; y < end; ++y, source_y += step, dest += FB_STRIDE) {
            int sy = source_y >> 16;
            int pixel_light = light;
            if (sy < 0) sy = 0;
            if (sy >= height) sy = height - 1;
            /* Fine one-pixel edge wholly inside this plane's polygon. */
            if (y < inner_start || y >= inner_end || far_edge)
                pixel_light = light * 7 >> 3;
            *dest = shade565(page_pixels[(ry + sy) * SCR_W + sx], pixel_light);
        }
    }
}

void skin_page_turn(float progress, int top, int bottom,
                    unsigned short background)
{
    (void)background;
    if (top < 0) top = 0;
    if (top > SCR_H) top = SCR_H;
    if (bottom < 0) bottom = 0;
    if (bottom > SCR_H) bottom = SCR_H;
    if (bottom <= top) return;
    skin_page_fold(progress, 0, top, SCR_W, bottom - top);
}
