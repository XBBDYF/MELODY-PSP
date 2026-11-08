/*
 * MELODY PSP — 二维码生成（QR Code，byte 模式，纠错等级 L，版本 1~5）
 * ==================================================================
 * 只为了一个用途：把网易云扫码登录的 URL 画到 PSP 屏幕上，
 * 让手机「扫一扫」直接扫，省得在 PSP 上手打 36 个字符。
 *
 * 实现范围（刻意收窄，够用且好验证）：
 *   · byte 模式（8 bit/字符），UTF-8 直接塞
 *   · 纠错等级 L，版本 1~5 单块（v5-L 能放 106 字节，登录 URL 才 66 字节）
 *   · 8 种掩码全算，按标准罚分挑最好的
 *   · 输出 1 bit/模块的方阵，交给 ui.c 逐点画
 *
 * 参考：ISO/IEC 18004。表都很小，直接抄在下面。
 */

#include "qr.h"

#include <string.h>

/* 版本 1..5，纠错等级 L：单块 */
static const int g_total_cw[6] = { 0, 26, 44, 70, 100, 134 };
static const int g_data_cw[6] = { 0, 19, 34, 55, 80, 108 };
static const int g_ec_cw[6] = { 0, 7, 10, 15, 20, 26 };

/* 对齐图形右下角坐标（版本 1 没有） */
static const int g_align_c2[6] = { 0, 0, 18, 22, 26, 30 };

/* ------------------------------------------------------------------ */
/* GF(256) 与 Reed-Solomon                                             */
/* ------------------------------------------------------------------ */

static unsigned char g_exp[512];
static unsigned char g_log[256];
static int g_gf_ready;

static void gf_init(void)
{
    int x = 1;

    if (g_gf_ready) return;
    g_gf_ready = 1;
    for (int i = 0; i < 255; ++i) {
        g_exp[i] = (unsigned char)x;
        g_log[x] = (unsigned char)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; ++i) g_exp[i] = g_exp[i - 255];
}

static unsigned char gf_mul(unsigned char a, unsigned char b)
{
    if (!a || !b) return 0;
    return g_exp[g_log[a] + g_log[b]];
}

/* 生成多项式系数（n 个，按 x^(n-1) .. x^0 顺序） */
static void rs_divisor(int n, unsigned char *out)
{
    unsigned char root = 1;

    memset(out, 0, (size_t)n);
    out[n - 1] = 1;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            out[j] = gf_mul(out[j], root);
            if (j + 1 < n) out[j] ^= out[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

static void rs_encode(const unsigned char *data, int dlen, unsigned char *ec, int ecn)
{
    unsigned char div[64];

    if (ecn <= 0 || ecn > 32) return;
    rs_divisor(ecn, div);
    memset(ec, 0, (size_t)ecn);

    for (int i = 0; i < dlen; ++i) {
        unsigned char factor = (unsigned char)(data[i] ^ ec[0]);

        memmove(ec, ec + 1, (size_t)(ecn - 1));
        ec[ecn - 1] = 0;
        for (int j = 0; j < ecn; ++j) ec[j] ^= gf_mul(div[j], factor);
    }
}

/* ------------------------------------------------------------------ */
/* 矩阵                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int n;
    unsigned char dark[57 * 57];
    unsigned char func[57 * 57];   /* 1 = 功能模块 */
} QrMat;

static void set_mod(QrMat *m, int x, int y, int v)
{
    if (x < 0 || y < 0 || x >= m->n || y >= m->n) return;
    m->dark[y * m->n + x] = (unsigned char)(v ? 1 : 0);
    m->func[y * m->n + x] = 1;
}

static void draw_finder(QrMat *m, int cx, int cy)
{
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            int x = cx + dx, y = cy + dy;
            int ax = dx < 0 ? -dx : dx;
            int ay = dy < 0 ? -dy : dy;
            int d, on;

            if (x < 0 || y < 0 || x >= m->n || y >= m->n) continue;
            d = ax > ay ? ax : ay;
            if (d == 4) on = 0;        /* 分隔带 */
            else if (d == 3) on = 1;   /* 外框 */
            else if (d == 2) on = 0;   /* 白圈 */
            else on = 1;               /* 3x3 实心 */
            set_mod(m, x, y, on);
        }
    }
}

static void draw_align(QrMat *m, int cx, int cy)
{
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int ax = dx < 0 ? -dx : dx;
            int ay = dy < 0 ? -dy : dy;

            set_mod(m, cx + dx, cy + dy, (ax == 2 || ay == 2 || (ax == 0 && ay == 0)));
        }
    }
}

static void draw_function(QrMat *m, int version)
{
    int n = m->n;

    memset(m->dark, 0, sizeof(m->dark));
    memset(m->func, 0, sizeof(m->func));

    draw_finder(m, 3, 3);
    draw_finder(m, n - 4, 3);
    draw_finder(m, 3, n - 4);

    /* 定位（timing） */
    for (int i = 8; i < n - 8; ++i) {
        set_mod(m, i, 6, (i & 1) == 0);
        set_mod(m, 6, i, (i & 1) == 0);
    }

    if (version >= 2) draw_align(m, g_align_c2[version], g_align_c2[version]);

    /* 格式信息区域先占位 */
    for (int i = 0; i <= 8; ++i) {
        if (i != 6) {
            set_mod(m, 8, i, 0);
            set_mod(m, i, 8, 0);
        }
    }
    for (int i = 0; i < 8; ++i) {
        set_mod(m, 8, n - 1 - i, 0);
        set_mod(m, n - 1 - i, 8, 0);
    }
    set_mod(m, 8, n - 8, 1);   /* 固定暗模块 */
    set_mod(m, 8, 6, 1);       /* timing 与格式信息交界处固定黑 */
    set_mod(m, 6, 8, 1);
}

/* ------------------------------------------------------------------ */
/* 位流 / 掩码 / 罚分                                                  */
/* ------------------------------------------------------------------ */

static void put_bits(unsigned char *buf, int *bitpos, unsigned int val, int n)
{
    for (int i = n - 1; i >= 0; --i) {
        int b = (int)((val >> i) & 1);

        if (b) buf[*bitpos >> 3] |= (unsigned char)(0x80 >> (*bitpos & 7));
        (*bitpos)++;
    }
}

static int mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return ((x + y) & 1) == 0;
    case 1: return (y & 1) == 0;
    case 2: return (x % 3) == 0;
    case 3: return ((x + y) % 3) == 0;
    case 4: return (((y / 2) + (x / 3)) & 1) == 0;
    case 5: return (((x * y) & 1) + ((x * y) % 3)) == 0;
    case 6: return ((((x * y) & 1) + ((x * y) % 3)) & 1) == 0;
    default: return ((((x + y) & 1) + ((x * y) % 3)) & 1) == 0;
    }
}

/* 15 位格式信息，level: L=1 M=0 Q=3 H=2 */
static unsigned int format_bits(int level, int mask)
{
    int data = (level << 3) | mask;
    int rem = data;

    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return (unsigned int)(((data << 10) | (rem & 0x3FF)) ^ 0x5412);
}

/* 把格式信息写到两个位置 */
static void draw_format(QrMat *m, unsigned int f)
{
    int n = m->n;

    for (int i = 0; i <= 5; ++i) m->dark[i * n + 8] = (unsigned char)((f >> i) & 1);
    m->dark[7 * n + 8] = (unsigned char)((f >> 6) & 1);
    m->dark[8 * n + 8] = (unsigned char)((f >> 7) & 1);
    m->dark[8 * n + 7] = (unsigned char)((f >> 8) & 1);
    for (int i = 9; i < 15; ++i) m->dark[8 * n + (14 - i)] = (unsigned char)((f >> i) & 1);

    for (int i = 0; i < 8; ++i) m->dark[8 * n + (n - 1 - i)] = (unsigned char)((f >> i) & 1);
    for (int i = 8; i < 15; ++i) m->dark[(n - 15 + i) * n + 8] = (unsigned char)((f >> i) & 1);
    m->dark[(n - 8) * n + 8] = 1;
}

static int penalty(const QrMat *m)
{
    int n = m->n, score = 0;

    for (int y = 0; y < n; ++y) {
        int run = 1;

        for (int x = 1; x < n; ++x) {
            if (m->dark[y * n + x] == m->dark[y * n + x - 1]) run++;
            else {
                if (run >= 5) score += 3 + (run - 5);
                run = 1;
            }
        }
        if (run >= 5) score += 3 + (run - 5);
    }
    for (int x = 0; x < n; ++x) {
        int run = 1;

        for (int y = 1; y < n; ++y) {
            if (m->dark[y * n + x] == m->dark[(y - 1) * n + x]) run++;
            else {
                if (run >= 5) score += 3 + (run - 5);
                run = 1;
            }
        }
        if (run >= 5) score += 3 + (run - 5);
    }
    for (int y = 0; y < n - 1; ++y) {
        for (int x = 0; x < n - 1; ++x) {
            unsigned char c = m->dark[y * n + x];

            if (c == m->dark[y * n + x + 1] && c == m->dark[(y + 1) * n + x] &&
                c == m->dark[(y + 1) * n + x + 1])
                score += 3;
        }
    }
    {
        int total = 0, pct, k;

        for (int i = 0; i < n * n; ++i) total += m->dark[i];
        pct = total * 100 / (n * n);
        k = pct / 5;
        if (k > 10) k = 10;
        score += (k - 5) * (k - 5) * 2;
    }
    return score;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int qr_encode(const char *text, unsigned char *out, int out_stride, int *out_dim)
{
    int len = (int)strlen(text);
    int version = 0;
    int n, total_cw, data_cw, ec_cw;
    unsigned char cw[256];
    unsigned char ec[64];
    unsigned char bits[256];
    int bitpos = 0;
    int best_mask = -1, best_score = 0x7FFFFFFF;
    QrMat best;

    if (!text || len <= 0 || len > 100) return -1;

    for (int v = 1; v <= 5; ++v) {
        if (len + 2 <= g_data_cw[v]) {
            version = v;
            break;
        }
    }
    if (!version) return -1;

    n = 17 + version * 4;
    total_cw = g_total_cw[version];
    data_cw = g_data_cw[version];
    ec_cw = g_ec_cw[version];

    gf_init();

    memset(bits, 0, sizeof(bits));
    put_bits(bits, &bitpos, 4, 4);                  /* byte 模式 */
    put_bits(bits, &bitpos, (unsigned int)len, 8);  /* 版本 1-9：8 位长度 */
    for (int i = 0; i < len; ++i) put_bits(bits, &bitpos, (unsigned char)text[i], 8);
    if (bitpos + 4 <= data_cw * 8) put_bits(bits, &bitpos, 0, 4);
    while (bitpos % 8) bitpos++;

    memcpy(cw, bits, (size_t)data_cw);
    for (int i = bitpos / 8, pad = 0; i < data_cw; ++i, ++pad)
        cw[i] = (pad & 1) ? 0x11 : 0xEC;

    rs_encode(cw, data_cw, ec, ec_cw);

    /* 拼最终码字流（数据 + 校验） */
    memset(bits, 0, sizeof(bits));
    bitpos = 0;
    for (int i = 0; i < total_cw; ++i) {
        unsigned char b = (i < data_cw) ? cw[i] : ec[i - data_cw];

        put_bits(bits, &bitpos, b, 8);
    }

    memset(&best, 0, sizeof(best));
    for (int mask = 0; mask < 8; ++mask) {
        QrMat cand;
        int bp = 0, score;

        memset(&cand, 0, sizeof(cand));
        cand.n = n;
        draw_function(&cand, version);

        {
            int right;

            for (right = n - 1; right >= 1; right -= 2) {
                int upward;

                if (right == 6) right = 5;
                upward = (((right + 1) & 2) == 0);
                for (int vert = 0; vert < n; ++vert) {
                    int y = upward ? (n - 1 - vert) : vert;

                    for (int j = 0; j < 2; ++j) {
                        int x = right - j;

                        if (cand.func[y * n + x]) continue;
                        {
                            int bit = (bp < total_cw * 8) &&
                                      ((bits[bp >> 3] >> (7 - (bp & 7))) & 1);

                            cand.dark[y * n + x] =
                                (unsigned char)(bit ^ mask_bit(mask, x, y));
                            bp++;
                        }
                    }
                }
            }
        }

        draw_format(&cand, format_bits(1, mask));
        score = penalty(&cand);
        if (score < best_score) {
            best_score = score;
            best_mask = mask;
            memcpy(&best, &cand, sizeof(best));
        }
    }

    if (best_mask < 0) return -1;
    (void)best_mask;

    /* 按边长紧凑存放（不用 out_stride）：调用方只关心 dim，看不到步长，
     * 之前按步长写、按 dim 读，屏幕上画出来就是一团乱码。 */
    (void)out_stride;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            out[y * n + x] = best.dark[y * n + x] ? 1 : 0;

    *out_dim = n;
    return n;
}
