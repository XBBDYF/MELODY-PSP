/*
 * MELODY PSP - 极简 PNG 解码（网易有些封面是 PNG，sceJpeg 解不了）
 * ------------------------------------------------------------------
 * 只支持：8 位、非隔行、颜色类型 0(灰度) / 2(RGB) / 3(调色板) / 6(RGBA)。
 * 覆盖网易封面 99% 的情况，输出统一成 RGBA8888，交给 rgba_to_565()。
 *
 * 复用项目已链接的 zlib，验证压缩流与 PNG chunk CRC；逐行反滤波后输出。
 */

#include "png.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* 本地音乐里内嵌封面常见 640x640，原来的 256 上限直接把图拒了 */
#define PNG_MAX_DIM 1024

/* Use the already-linked zlib implementation rather than a second custom
 * DEFLATE parser. uncompress validates the zlib header and Adler checksum. */
static int zlib_inflate(const unsigned char *src, int slen, unsigned char *dst, int dmax)
{
    uLongf size = (uLongf)dmax;
    if (slen < 0 || dmax < 0 || uncompress(dst, &size, src, (uLong)slen) != Z_OK)
        return -1;
    return size <= (uLongf)dmax ? (int)size : -1;
}

static unsigned int be32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* 未滤波：bpp 是每像素字节数 */
static int unfilter(unsigned char *raw, int w, int h, int bpp)
{
    int stride = w * bpp;
    unsigned char *prev = NULL;

    for (int y = 0; y < h; ++y) {
        unsigned char *line = raw + (long long)y * (stride + 1);
        int ft = line[0];
        unsigned char *cur = line + 1;

        switch (ft) {
        case 0:
            break;
        case 1:
            for (int i = bpp; i < stride; ++i) cur[i] = (unsigned char)(cur[i] + cur[i - bpp]);
            break;
        case 2:
            if (prev) {
                for (int i = 0; i < stride; ++i) cur[i] = (unsigned char)(cur[i] + prev[i]);
            }
            break;
        case 3:
            for (int i = 0; i < stride; ++i) {
                int a = (i >= bpp) ? cur[i - bpp] : 0;
                int up = prev ? prev[i] : 0;

                cur[i] = (unsigned char)(cur[i] + ((a + up) >> 1));
            }
            break;
        case 4:
            for (int i = 0; i < stride; ++i) {
                int a = (i >= bpp) ? cur[i - bpp] : 0;
                int up = prev ? prev[i] : 0;
                int c = (prev && i >= bpp) ? prev[i - bpp] : 0;

                cur[i] = (unsigned char)(cur[i] + paeth(a, up, c));
            }
            break;
        default:
            return -1;
        }
        prev = cur;
    }
    return 0;
}

static int png_decode_impl(const unsigned char *src, int len, unsigned char *out, int out_max,
                            int max_dim, int *w_out, int *h_out, unsigned char *workspace, int workspace_size)
{
    unsigned char *zdata = NULL;
    int zcap = 0;
    int saw_header = 0, saw_end = 0;
    unsigned char *raw = NULL;
    int zlen = 0;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 1;
    const unsigned char *plte = NULL;
    int plte_n = 0;
    int pos = 8;    /* 跳过 PNG 签名 */
    int rawlen, bpp, stride;

    if (!src || !out || len < 8 || out_max < 0 || memcmp(src, "\x89PNG\r\n\x1a\n", 8)) return -1;
    if (w_out) *w_out = 0;
    if (h_out) *h_out = 0;

    while (pos <= len - 12) {
        unsigned int clen = be32(src + pos);
        const unsigned char *type = src + pos + 4;
        const unsigned char *data = src + pos + 8;

        /* Subtraction before casting prevents signed overflow and OOB memcpy. */
        if (clen > (unsigned)(len - pos - 12)) goto done;
        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, type, 4);
        crc = crc32(crc, data, clen);
        if ((unsigned long)be32(data + clen) != crc) goto done;
        if (!saw_header && memcmp(type, "IHDR", 4)) goto done;

        if (!memcmp(type, "IHDR", 4)) {
            if (saw_header || clen != 13 || data[10] || data[11]) goto done;
            saw_header = 1;
            w = (int)be32(data);
            h = (int)be32(data + 4);
            depth = data[8];
            ctype = data[9];
            interlace = data[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            if (!clen || clen > 768 || clen % 3) goto done;
            plte = data;
            plte_n = (int)(clen / 3);
        } else if (!memcmp(type, "IDAT", 4)) {
            if (clen > 8u * 1024u * 1024u - (unsigned)zlen) goto done;
            if (zlen + (int)clen > zcap) {
                int nc = zlen + (int)clen + 8192;
                unsigned char *nz = (unsigned char *)realloc(zdata, (size_t)nc);

                if (!nz) goto done;
                zdata = nz;
                zcap = nc;
            }
            memcpy(zdata + zlen, data, clen);
            zlen += (int)clen;
        } else if (!memcmp(type, "IEND", 4)) {
            if (clen) goto done;
            saw_end = 1;
            break;
        } else if (!(type[0] & 32)) {
            goto done; /* unknown critical chunk */
        }
        pos += 8 + (int)clen + 4;
    }

    if (!saw_header || !saw_end || w <= 0 || h <= 0 || w > PNG_MAX_DIM || h > PNG_MAX_DIM) goto done;
    if (depth != 8 || interlace != 0) goto done;
    if (ctype != 0 && ctype != 2 && ctype != 3 && ctype != 6) goto done;
    if (zlen <= 0 || (ctype == 3 && !plte)) goto done;

    bpp = (ctype == 6) ? 4 : (ctype == 2) ? 3 : 1;
    stride = w * bpp;
    rawlen = (stride + 1) * h;
    if (rawlen > 8 * 1024 * 1024) goto done;

    raw=workspace&&workspace_size>=rawlen?workspace:(workspace?NULL:(unsigned char*)malloc((size_t)rawlen));
    if (!raw) goto done;
    if (zlib_inflate(zdata, zlen, raw, rawlen) != rawlen) goto done;
    if (unfilter(raw, w, h, bpp) < 0) goto done;

    /* 大于 max_dim：解码时直接盒式缩小，省掉 1.6MB 的满尺寸临时缓冲 */
    if (max_dim > 0 && (w > max_dim || h > max_dim)) {
        int m = (w > h) ? w : h;
        int dw = (int)((long long)w * max_dim / m);
        int dh = (int)((long long)h * max_dim / m);

        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if ((long long)dw * dh * 4 > out_max) goto done;

        for (int y = 0; y < dh; ++y) {
            int y0 = y * h / dh, y1 = (y + 1) * h / dh;

            if (y1 <= y0) y1 = y0 + 1;
            if (y1 > h) y1 = h;
            for (int x = 0; x < dw; ++x) {
                int x0 = x * w / dw, x1 = (x + 1) * w / dw;
                int sr = 0, sg = 0, sb = 0, sa = 0, n = 0;

                if (x1 <= x0) x1 = x0 + 1;
                if (x1 > w) x1 = w;
                for (int yy = y0; yy < y1; ++yy) {
                    const unsigned char *line = raw + (long long)yy * (stride + 1) + 1;

                    for (int xx = x0; xx < x1; ++xx) {
                        int r, g, b, a = 255;

                        if (ctype == 6) {
                            r = line[xx * 4];
                            g = line[xx * 4 + 1];
                            b = line[xx * 4 + 2];
                            a = line[xx * 4 + 3];
                        } else if (ctype == 2) {
                            r = line[xx * 3];
                            g = line[xx * 3 + 1];
                            b = line[xx * 3 + 2];
                        } else if (ctype == 3) {
                            int idx = line[xx];

                            if (!plte || idx >= plte_n) { r = g = b = 0; }
                            else {
                                r = plte[idx * 3];
                                g = plte[idx * 3 + 1];
                                b = plte[idx * 3 + 2];
                            }
                        } else {
                            r = g = b = line[xx];
                        }
                        sr += r;
                        sg += g;
                        sb += b;
                        sa += a;
                        n++;
                    }
                }
                if (n <= 0) n = 1;
                out[((long long)y * dw + x) * 4] = (unsigned char)(sr / n);
                out[((long long)y * dw + x) * 4 + 1] = (unsigned char)(sg / n);
                out[((long long)y * dw + x) * 4 + 2] = (unsigned char)(sb / n);
                out[((long long)y * dw + x) * 4 + 3] = (unsigned char)(sa / n);
            }
        }
        if (w_out) *w_out = dw;
        if (h_out) *h_out = dh;
        if(raw!=workspace)free(raw);
        free(zdata);
        return 0;
    }

    /* 转 RGBA8888 */
    if ((long long)w * h * 4 > out_max) goto done;

    for (int y = 0; y < h; ++y) {
        const unsigned char *line = raw + (long long)y * (stride + 1) + 1;
        unsigned char *dst = out + (long long)y * w * 4;

        for (int x = 0; x < w; ++x) {
            unsigned char r, g, b, a = 255;

            if (ctype == 6) {
                r = line[x * 4];
                g = line[x * 4 + 1];
                b = line[x * 4 + 2];
                a = line[x * 4 + 3];
            } else if (ctype == 2) {
                r = line[x * 3];
                g = line[x * 3 + 1];
                b = line[x * 3 + 2];
            } else if (ctype == 3) {
                int idx = line[x];

                if (!plte || idx >= plte_n) { r = g = b = 0; }
                else {
                    r = plte[idx * 3];
                    g = plte[idx * 3 + 1];
                    b = plte[idx * 3 + 2];
                }
            } else {
                r = g = b = line[x];
            }

            dst[x * 4] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }

    if (w_out) *w_out = w;
    if (h_out) *h_out = h;
    if(raw!=workspace)free(raw);
    free(zdata);
    return 0;

done:
    if(raw!=workspace)free(raw);
    free(zdata);
    return -1;
}

int png_decode_rgba(const unsigned char *src, int len, unsigned char *out, int out_max,
                    int *w_out, int *h_out)
{
    return png_decode_impl(src, len, out, out_max, 0, w_out, h_out, NULL,0);
}

int png_decode_rgba_scaled(const unsigned char *src, int len, unsigned char *out, int max_dim,
                           int *w_out, int *h_out)
{
    if (max_dim <= 0 || max_dim > PNG_MAX_DIM) return -1;
    return png_decode_impl(src, len, out, max_dim * max_dim * 4, max_dim, w_out, h_out,NULL,0);
}

int png_decode_rgba_workspace(const unsigned char*src,int len,unsigned char*out,int out_max,int*w,int*h,unsigned char*work,int size){return png_decode_impl(src,len,out,out_max,0,w,h,work,size);}
