/*
 * MELODY PSP - 纯软件 baseline JPEG 解码器
 * ==================================================================
 * 为什么需要它：
 *   PSP 的 sceJpeg（硬件 MJPG 解码）只认视频帧那么大的图（实机实测 640x640
 *   直接返回 0x80650020 SCE_JPEG_ERROR_UNSUPPORT_IMAGE_SIZE），而本地音乐的
 *   内嵌封面几乎清一色 640x640，于是「本地音乐没有封面」。
 *
 * 实现范围（够用且安全）：
 *   · baseline / extended-sequential（SOF0/SOF1）单扫描 interleaved
 *   · DQT / DHT / SOF / DRI / SOS，任意 4:4:4 / 4:2:2 / 4:2:0 / 灰度
 *   · Huffman：9 位快表 + 长码规范回退
 *   · 整数 8x8 IDCT（分离式，13 位定点常数，系数钳到 ±4096 防溢出）
 *   · 输出 RGBA8888；超过 max_dim 先解满尺寸临时图再盒式缩小
 *
 * 不支持渐进式（SOF2）：直接返回错误，由调用方决定要不要显示占位图。
 * 不依赖 PSPSDK，可以在 PC 上单独编译做单元测试。
 */

#include "jpegdec.h"

#include <stdlib.h>
#include <string.h>

#ifdef JPEGDEC_TRACE
#include <stdio.h>
#define JT(...) fprintf(stderr, __VA_ARGS__)
#else
#define JT(...) ((void)0)
#endif

#define JPEG_TMP_MAX_BYTES (4 * 1024 * 1024)   /* 满尺寸临时缓冲上限 */
#define JPEG_MAX_DIM       4096

/* ------------------------------------------------------------------ */
/* 定点常数                                                            */
/* ------------------------------------------------------------------ */

/* c[u][x] = round(8192 * C(u) * cos((2x+1)u*pi/16)), C(0) = 1/sqrt2 */
static const int g_idct[8][8] = {
    {  5793,  5793,  5793,  5793,  5793,  5793,  5793,  5793 },
    {  8035,  6811,  4551,  1598, -1598, -4551, -6811, -8035 },
    {  7568,  3135, -3135, -7568, -7568, -3135,  3135,  7568 },
    {  6811, -1598, -8035, -4551,  4551,  8035,  1598, -6811 },
    {  5793, -5793, -5793,  5793,  5793, -5793, -5793,  5793 },
    {  4551, -8035,  1598,  6811, -6811, -1598,  8035, -4551 },
    {  3135, -7568,  7568, -3135, -3135,  7568, -7568,  3135 },
    {  1598, -4551,  6811, -8035,  8035, -6811,  4551, -1598 },
};

static const unsigned char g_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/* ------------------------------------------------------------------ */
/* 结构                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char *p;
    const unsigned char *end;
    unsigned int buf;
    int cnt;
    int bad;
    int eof;
} BitReader;

typedef struct {
    int valid;
    unsigned char bits[17];
    unsigned char val[256];
    unsigned short lut[512];       /* 9 位快表：0 = 未命中，(len<<8)|val */
    int mincode[17], maxcode[17], valptr[17];
} Huff;

typedef struct {
    int id;
    int hs, vs;
    int tq;
    int td, ta;
    int pw, ph;
    unsigned char *plane;
    int pred;
} Comp;

typedef struct {
    int w, h;
    int ncomp;
    Comp c[4];
    int max_h, max_v;
    int q[4][64];
    int qvalid[4];
    Huff dc[4], ac[4];
    int ri;
    int mcus_x, mcus_y;
} Jpeg;

/* ------------------------------------------------------------------ */
/* 位读取                                                              */
/* ------------------------------------------------------------------ */

/* JPEG 熵编码结束后的填充位按规范全是 1。这里遇到 marker / 文件尾就补 1，
 * 否则最后一个 block 会在「差几个 bit」的地方解出错码（实测 640x640 封面
 * 就坏在最后一个 MCU 的 Cr 上）。 */
static int br_byte(BitReader *b)
{
    int c, c2;

    if (b->p >= b->end) {
        b->eof = 1;
        return 0xFF;
    }
    c = *b->p++;
    if (c != 0xFF) return c;

    if (b->p >= b->end) {
        b->eof = 1;
        return 0xFF;
    }
    c2 = *b->p;
    if (c2 == 0x00) {
        b->p++;
        return 0xFF;
    }
    if (c2 == 0xFF) {
        while (b->p < b->end && *b->p == 0xFF) b->p++;
        if (b->p < b->end && *b->p == 0x00) {
            b->p++;
            return 0xFF;
        }
    }
    /* 撞到 marker：熵数据结束，把 p 留在 marker 起点，之后一直补 1 */
    b->p--;
    b->eof = 1;
    return 0xFF;
}

static void br_fill(BitReader *b)
{
    while (b->cnt <= 16) {
        b->buf = (b->buf << 8) | (unsigned int)(br_byte(b) & 0xFF);
        b->cnt += 8;
        if (b->eof) break;
    }
}

static int br_bits(BitReader *b, int n)
{
    int v;

    if (n <= 0) return 0;
    if (b->cnt < n) br_fill(b);
    if (b->cnt < n) {
        b->bad = 1;
        return 0;
    }
    b->cnt -= n;
    v = (int)((b->buf >> b->cnt) & ((1u << n) - 1));
    return v;
}

/* ------------------------------------------------------------------ */
/* Huffman                                                             */
/* ------------------------------------------------------------------ */

static void huff_build(Huff *h)
{
    int code = 0, k = 0;

    memset(h->lut, 0, sizeof(h->lut));
    memset(h->maxcode, 0, sizeof(h->maxcode));

    for (int len = 1; len <= 16; ++len) {
        int n = h->bits[len];

        h->valptr[len] = k;
        h->mincode[len] = code;
        code += n;
        h->maxcode[len] = n ? code - 1 : -1;
        k += n;
        code <<= 1;
    }

    code = 0;
    k = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int i = 0; i < h->bits[len]; ++i) {
            if (len <= 9) {
                int shift = 9 - len;
                int base = code << shift;
                unsigned short e = (unsigned short)((len << 8) | h->val[k]);

                for (int j = 0; j < (1 << shift); ++j) h->lut[base + j] = e;
            }
            code++;
            k++;
        }
        code <<= 1;
    }
    h->valid = 1;
}

static int huff_decode(BitReader *b, const Huff *h)
{
    int code = 0;

    if (!h->valid) return -1;
    if (b->cnt < 16) br_fill(b);

    if (b->cnt >= 9) {
        unsigned short e = h->lut[(b->buf >> (b->cnt - 9)) & 511];

        if (e) {
            int len = e >> 8;

            if (len <= b->cnt) {
                b->cnt -= len;
                return e & 0xFF;
            }
        }
    } else if (b->cnt > 0) {
        unsigned short e = h->lut[(b->buf << (9 - b->cnt)) & 511];

        if (e) {
            int len = e >> 8;

            if (len <= b->cnt) {
                b->cnt -= len;
                return e & 0xFF;
            }
        }
    }

    for (int len = 1; len <= 16; ++len) {
        code = (code << 1) | br_bits(b, 1);
        if (b->bad) return -1;
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len])
            return h->val[h->valptr[len] + code - h->mincode[len]];
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* IDCT                                                                */
/* ------------------------------------------------------------------ */

static void idct_block(const int *coef, unsigned char *dst, int stride)
{
    int tmp[64];

    for (int y = 0; y < 8; ++y) {
        const int *row = coef + y * 8;
        int *orow = tmp + y * 8;

        for (int x = 0; x < 8; ++x) {
            int s = 0;

            for (int u = 0; u < 8; ++u) s += g_idct[u][x] * row[u];
            orow[x] = (s + 8192) >> 14;
        }
    }

    for (int x = 0; x < 8; ++x) {
        unsigned char *col = dst + x;

        for (int y = 0; y < 8; ++y) {
            int s = 0, v;

            for (int k = 0; k < 8; ++k) s += g_idct[k][y] * tmp[k * 8 + x];
            v = ((s + 8192) >> 14) + 128;
            if (v < 0) v = 0;
            else if (v > 255) v = 255;
            col[y * stride] = (unsigned char)v;
        }
    }
}

/* ------------------------------------------------------------------ */
/* segment 解析                                                        */
/* ------------------------------------------------------------------ */

static int parse_dqt(Jpeg *j, const unsigned char *p, int len)
{
    int pos = 0;

    while (pos < len) {
        int pq = p[pos] >> 4, tq = p[pos] & 15, n = pq ? 128 : 64;

        pos++;
        if (tq > 3 || pos + n > len) return -1;
        for (int i = 0; i < 64; ++i)
            j->q[tq][i] = pq ? ((p[pos + i * 2] << 8) | p[pos + i * 2 + 1]) : p[pos + i];
        j->qvalid[tq] = 1;
        pos += n;
    }
    return 0;
}

static int parse_dht(Jpeg *j, const unsigned char *p, int len)
{
    int pos = 0;

    while (pos + 17 <= len) {
        int tc = p[pos] >> 4, th = p[pos] & 15, total = 0;
        Huff *h;

        if (tc > 1 || th > 3) return -1;
        h = tc ? &j->ac[th] : &j->dc[th];
        memset(h->bits, 0, sizeof(h->bits));
        for (int i = 1; i <= 16; ++i) {
            h->bits[i] = p[pos + i];
            total += h->bits[i];
        }
        pos += 17;
        if (total > 256 || pos + total > len) return -1;
        for (int i = 0; i < total; ++i) h->val[i] = p[pos + i];
        huff_build(h);
        pos += total;
    }
    return 0;
}

static int parse_sof(Jpeg *j, const unsigned char *p, int len, int marker)
{
    int n, w, h;

    if (len < 8) return -1;
    n = p[7];
    if (n < 1 || n > 4) return -1;
    if (len < 8 + n * 3) return -1;

    h = (p[3] << 8) | p[4];
    w = (p[5] << 8) | p[6];
    if (w <= 0 || h <= 0 || w > JPEG_MAX_DIM || h > JPEG_MAX_DIM) return -1;

    j->w = w;
    j->h = h;
    j->ncomp = n;
    j->max_h = j->max_v = 1;

    for (int i = 0; i < n; ++i) {
        const unsigned char *c = p + 8 + i * 3;
        Comp *cp = &j->c[i];

        cp->id = c[0];
        cp->hs = c[1] >> 4;
        cp->vs = c[1] & 15;
        cp->tq = c[2];
        cp->td = cp->ta = 0;
        cp->pred = 0;
        cp->plane = NULL;
        if (cp->hs < 1 || cp->hs > 4 || cp->vs < 1 || cp->vs > 4) return -1;
        if (cp->hs > j->max_h) j->max_h = cp->hs;
        if (cp->vs > j->max_v) j->max_v = cp->vs;
    }
    (void)marker;
    return 0;
}

static int alloc_planes(Jpeg *j)
{
    j->mcus_x = (j->w + j->max_h * 8 - 1) / (j->max_h * 8);
    j->mcus_y = (j->h + j->max_v * 8 - 1) / (j->max_v * 8);

    for (int i = 0; i < j->ncomp; ++i) {
        Comp *cp = &j->c[i];

        cp->pw = j->mcus_x * cp->hs * 8;
        cp->ph = j->mcus_y * cp->vs * 8;
        cp->plane = (unsigned char *)calloc((size_t)cp->pw * cp->ph, 1);
        if (!cp->plane) return -1;
    }
    return 0;
}

static void free_planes(Jpeg *j)
{
    for (int i = 0; i < 4; ++i) {
        if (j->c[i].plane) {
            free(j->c[i].plane);
            j->c[i].plane = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 解码                                                                */
/* ------------------------------------------------------------------ */

static int decode_block(Jpeg *j, BitReader *br, Comp *cp, int *blk)
{
    const Huff *dc = &j->dc[cp->td];
    const Huff *ac = &j->ac[cp->ta];
    const int *q = j->q[cp->tq];
    int t, k;

    memset(blk, 0, sizeof(int) * 64);

    t = huff_decode(br, dc);
    if (t < 0 || t > 11) { JT("dc fail t=%d cnt=%d\n", t, br->cnt); return -1; }
    if (t) {
        int diff = br_bits(br, t);

        if (br->bad) return -1;
        if (diff < (1 << (t - 1))) diff -= (1 << t) - 1;
        cp->pred += diff;
    }
    {
        int d = cp->pred * q[0];

        if (d > 4096) d = 4096;
        else if (d < -4096) d = -4096;
        blk[0] = d;
    }

    for (k = 1; k < 64; ) {
        int rs = huff_decode(br, ac);
        int r, sz;

        if (rs < 0) { JT("ac fail k=%d cnt=%d\n", k, br->cnt); return -1; }
        r = rs >> 4;
        sz = rs & 15;
        if (sz == 0) {
            if (r != 15) break;          /* EOB */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) { JT("k>63 k=%d r=%d\n", k, r); return -1; }
        {
            int v = br_bits(br, sz);

            if (br->bad) return -1;
            if (v < (1 << (sz - 1))) v -= (1 << sz) - 1;
            v *= q[k];
            if (v > 4096) v = 4096;
            else if (v < -4096) v = -4096;
            blk[g_zigzag[k]] = v;
        }
        k++;
    }
    return 0;
}

static int skip_rst(BitReader *br)
{
    while (br->p + 1 < br->end) {
        if (br->p[0] == 0xFF && br->p[1] >= 0xD0 && br->p[1] <= 0xD7) {
            br->p += 2;
            br->bad = 0;
            br->eof = 0;
            br->cnt = 0;
            br->buf = 0;
            return 0;
        }
        br->p++;
    }
    return -1;
}

static int decode_scan(Jpeg *j, BitReader *br)
{
    int blk[64];
    int rst = 0;

    for (int my = 0; my < j->mcus_y; ++my) {
        for (int mx = 0; mx < j->mcus_x; ++mx) {
            for (int ci = 0; ci < j->ncomp; ++ci) {
                Comp *cp = &j->c[ci];

                for (int vy = 0; vy < cp->vs; ++vy) {
                    for (int hx = 0; hx < cp->hs; ++hx) {
                        if (decode_block(j, br, cp, blk) < 0) { JT("block fail mcu=%d,%d ci=%d\n", mx, my, ci); return -1; }
                        idct_block(blk, cp->plane + (my * cp->vs + vy) * 8 * cp->pw +
                                            (mx * cp->hs + hx) * 8, cp->pw);
                    }
                }
            }
            if (j->ri > 0 && ++rst >= j->ri) {
                rst = 0;
                if (skip_rst(br) < 0) return -1;
                for (int ci = 0; ci < j->ncomp; ++ci) j->c[ci].pred = 0;
            }
        }
    }
    return 0;
}

/* 满尺寸 RGBA8888 */
static void emit_rgba(Jpeg *j, unsigned char *dst)
{
    const Comp *cy = &j->c[0];
    const Comp *cb = (j->ncomp >= 3) ? &j->c[1] : NULL;
    const Comp *cr = (j->ncomp >= 3) ? &j->c[2] : NULL;

    for (int y = 0; y < j->h; ++y) {
        unsigned char *o = dst + (size_t)y * j->w * 4;
        int yrow = y * cy->vs / j->max_v;
        int cbrow = cb ? (y * cb->vs / j->max_v) : 0;
        int crrow = cr ? (y * cr->vs / j->max_v) : 0;

        for (int x = 0; x < j->w; ++x) {
            int yv = cy->plane[yrow * cy->pw + x * cy->hs / j->max_h];
            int r, g, b;

            if (!cb) {
                r = g = b = yv;
            } else {
                int cbv = cb->plane[cbrow * cb->pw + x * cb->hs / j->max_h] - 128;
                int crv = cr->plane[crrow * cr->pw + x * cr->hs / j->max_h] - 128;
                int rr = yv + ((91881 * crv) >> 16);
                int gg = yv - ((22554 * cbv + 46802 * crv) >> 16);
                int bb = yv + ((116130 * cbv) >> 16);

                r = rr < 0 ? 0 : (rr > 255 ? 255 : rr);
                g = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
                b = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
            }
            o[x * 4] = (unsigned char)r;
            o[x * 4 + 1] = (unsigned char)g;
            o[x * 4 + 2] = (unsigned char)b;
            o[x * 4 + 3] = 255;
        }
    }
}

static void box_scale_rgba(const unsigned char *src, int sw, int sh,
                           unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; ++y) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;

        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (int x = 0; x < dw; ++x) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            int r = 0, g = 0, b = 0, n = 0;

            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            for (int yy = y0; yy < y1; ++yy) {
                const unsigned char *row = src + ((size_t)yy * sw + x0) * 4;

                for (int xx = x0; xx < x1; ++xx) {
                    r += row[0];
                    g += row[1];
                    b += row[2];
                    row += 4;
                    n++;
                }
            }
            if (n <= 0) n = 1;
            dst[((size_t)y * dw + x) * 4] = (unsigned char)(r / n);
            dst[((size_t)y * dw + x) * 4 + 1] = (unsigned char)(g / n);
            dst[((size_t)y * dw + x) * 4 + 2] = (unsigned char)(b / n);
            dst[((size_t)y * dw + x) * 4 + 3] = 255;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int jpeg_read_size(const unsigned char *jpg, int len, int *w, int *h, int *progressive)
{
    const unsigned char *p, *end;
    int i;

    if (!jpg || len < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) return -1;
    p = jpg + 2;
    end = jpg + len;

    for (i = 0; i < 4096 && p + 4 <= end; ++i) {
        int m, ln;

        if (p[0] != 0xFF) {
            p++;
            continue;
        }
        m = p[1];
        if (m == 0xFF || m == 0x00) {
            p++;
            continue;
        }
        if (m == 0xD9) break;
        ln = (p[2] << 8) | p[3];
        if (ln < 2 || p + ln > end) break;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            int n = p[9];

            if (n < 1 || ln < 8 + n * 3) return -1;
            if (w) *w = (p[7] << 8) | p[8];
            if (h) *h = (p[5] << 8) | p[6];
            if (progressive) *progressive = (m == 0xC2 || m == 0xC6 || m == 0xCA);
            return 0;
        }
        p += ln;
    }
    return -1;
}

int jpeg_decode_rgba_scaled(const unsigned char *jpg, int len, unsigned char *out,
                            int max_dim, int *ow, int *oh)
{
    Jpeg j;
    const unsigned char *p, *end;
    int rc = -1, decoded = 0;
    unsigned char *full = NULL, *tmp = NULL;

    if (!jpg || len < 4 || !out || max_dim < 8) return -1;
    if (jpg[0] != 0xFF || jpg[1] != 0xD8) return -1;

    memset(&j, 0, sizeof(j));
    p = jpg + 2;
    end = jpg + len;

    for (int guard = 0; guard < 8192 && p + 4 <= end; ++guard) {
        int m, ln;

        if (p[0] != 0xFF) {
            p++;
            continue;
        }
        m = p[1];
        if (m == 0xFF || m == 0x00) {
            p++;
            continue;
        }
        p += 2;
        JT("marker %02X at %d\n", m, (int)(p - jpg));
        if (m == 0xD9) break;
        if (p + 2 > end) break;
        ln = (p[0] << 8) | p[1];
        if (ln < 2 || p + ln > end) break;

        switch (m) {
        case 0xDB:
            if (parse_dqt(&j, p + 2, ln - 2) < 0) { JT("dqt fail\n"); goto done; }
            break;
        case 0xC4:
            if (parse_dht(&j, p + 2, ln - 2) < 0) { JT("dht fail\n"); goto done; }
            break;
        case 0xDD:
            if (ln >= 4) j.ri = (p[2] << 8) | p[3];
            break;
        case 0xC0:
        case 0xC1:
            if (parse_sof(&j, p, ln, m) < 0) { JT("sof fail ln=%d\n", ln); goto done; }
            if (alloc_planes(&j) < 0) { JT("alloc fail\n"); goto done; }
            JT("sof %dx%d ncomp=%d max=%dx%d mcu=%dx%d\n", j.w, j.h, j.ncomp, j.max_h, j.max_v, j.mcus_x, j.mcus_y);
            break;
        case 0xC2:
        case 0xC3:
        case 0xC5:
        case 0xC6:
        case 0xC7:
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCD:
        case 0xCE:
        case 0xCF:
            rc = -3;                        /* 渐进式/无损：不支持 */
            goto done;
        case 0xDA: {
            BitReader br;
            int ns = p[2];

            if (!j.ncomp || j.w <= 0) goto done;
            if (ns != j.ncomp) {
                rc = -4;                    /* 只支持 interleaved 单扫描 */
                goto done;
            }
            /* 每个分量的 Huffman 表选择子在 SOS 里，不在 SOF 里 */
            for (int i = 0; i < ns; ++i) {
                int cs = p[3 + i * 2];      /* p 指向长度字段：p[3]=Cs, p[4]=Td/Ta */
                int tt = p[4 + i * 2];

                for (int k = 0; k < j.ncomp; ++k) {
                    if (j.c[k].id == cs) {
                        j.c[k].td = tt >> 4;
                        j.c[k].ta = tt & 15;
                    }
                }
            }
            br.p = p + ln;
            br.end = end;
            br.buf = 0;
            br.cnt = 0;
            br.bad = 0;
            br.eof = 0;
            if (decode_scan(&j, &br) < 0) { JT("scan fail at mcuidx (see block)\n"); goto done; }
            decoded = 1;
            break;
        }
        default:
            break;
        }
        if (decoded) break;
        p += ln;
    }

    if (!decoded) goto done;

    {
        int w = j.w, h = j.h;
        int dw, dh;
        long need = (long)w * h * 4;
        long m = (w > h) ? w : h;

        dw = (int)((long)w * max_dim / m);
        dh = (int)((long)h * max_dim / m);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        if (w <= max_dim && h <= max_dim) {
            emit_rgba(&j, out);
            if (ow) *ow = w;
            if (oh) *oh = h;
            rc = 0;
            goto done;
        }
        if (need > JPEG_TMP_MAX_BYTES) {
            rc = -2;                        /* 图太大 */
            goto done;
        }
        full = (unsigned char *)malloc((size_t)need);
        if (!full) {
            rc = -2;
            goto done;
        }
        emit_rgba(&j, full);
        box_scale_rgba(full, w, h, out, dw, dh);
        if (ow) *ow = dw;
        if (oh) *oh = dh;
        rc = 0;
    }

done:
    if (tmp) free(tmp);
    if (full) free(full);
    free_planes(&j);
    return rc;
}
