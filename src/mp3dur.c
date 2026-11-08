/*
 * MELODY PSP — 本地 MP3 时长（精确到帧）
 * ------------------------------------------------------------------
 * 为什么不用 audio_probe_duration()/audio_dur_ms()：
 *   那个函数拿「第一帧的码率」去乘文件长度，VBR 文件能差 30~80%
 *   （实测 56 个文件里 44 个 VBR，18 个误差 >12 秒）。
 *   本地歌曲补封面/歌词要靠时长做硬门槛，时长不准就会把对的歌拒掉、
 *   或者把翻唱/Live 当成原版。
 *
 * 这里按 MPEG 帧头逐帧累计：
 *   - 有 Xing/Info 头（LAME 常见）直接用帧数算，精确；
 *   - 没有就取 3 个位置各 240 帧的平均码率估算。
 * 代码来自 tools/probe/lm_core.c（已在 PC 上与 Python 参考实现对齐、
 * 52/56 精确、最大误差 0.6 秒）。
 */

#include <stdio.h>
#include <string.h>

#include "mp3dur.h"

static int frame_hdr(const unsigned char *b, int n, int p,
                     int *spf, int *br, int *sr, int *pad, int *mono)
{
    static const int BR1[15] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
    static const int BR2[15] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
    static const int SR1[3] = { 44100, 48000, 32000 };
    static const int SR2[3] = { 22050, 24000, 16000 };
    static const int SR3[3] = { 11025, 12000, 8000 };
    int ver, layer, bri, sri;

    if (p + 4 > n) return 0;
    if (b[p] != 0xFF || (b[p + 1] & 0xE0) != 0xE0) return 0;
    ver = (b[p + 1] >> 3) & 3;
    layer = (b[p + 1] >> 1) & 3;
    bri = (b[p + 2] >> 4) & 0x0F;
    sri = (b[p + 2] >> 2) & 3;
    if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) return 0;
    *br = ((ver == 3) ? BR1[bri] : BR2[bri]) * 1000;
    *sr = (ver == 3) ? SR1[sri] : (ver == 2 ? SR2[sri] : SR3[sri]);
    *spf = (ver == 3) ? 1152 : 576;
    *pad = (b[p + 2] >> 1) & 1;
    *mono = ((b[p + 3] >> 6) == 3);
    return 1;
}

/* 在一个窗口里找一个可信的帧起点（后继帧也对齐才算数）；-1 = 没找到 */
static int sync_frame(const unsigned char *b, int n, int limit,
                      int *spf, int *br, int *sr, int *pad, int *mono)
{
    for (int p = 0; p + 4 <= n && p < limit; ++p) {
        int L, s2, b2, r2, pd2, m2;

        if (!frame_hdr(b, n, p, spf, br, sr, pad, mono)) continue;
        L = *spf / 8 * *br / *sr + *pad;
        if (L < 24) continue;
        if (p + L + 4 <= n && !frame_hdr(b, n, p + L, &s2, &b2, &r2, &pd2, &m2))
            continue;
        return p;
    }
    return -1;
}

int mp3_dur_ms(const char *path, int *conf)
{
    static unsigned char win[8192];
    unsigned char hdr[10];
    FILE *fp;
    long size;
    long long audio;
    int start = 0, n, p, spf = 0, br = 0, sr = 0, pad = 0, mono = 0;
    long long bytes = 0, usum = 0;
    int cnt = 0, distinct = 0, first_br = 0;

    if (conf) *conf = 0;
    if (!path || !path[0]) return 0;
    fp = fopen(path, "rb");
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    size = ftell(fp);
    if (size < 4096) { fclose(fp); return 0; }
    fseek(fp, 0, SEEK_SET);
    if (fread(hdr, 1, 10, fp) == 10 && !memcmp(hdr, "ID3", 3)) {
        start = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
        start += 10;
        if (start >= size - 1024) start = 0;
    }
    audio = (long long)size - start;      /* 必须 64 位：PSP 的 long 是 32 位 */
    if (fseek(fp, start, SEEK_SET) != 0) { fclose(fp); return 0; }
    n = (int)fread(win, 1, sizeof(win), fp);
    p = sync_frame(win, n, n, &spf, &br, &sr, &pad, &mono);
    if (p < 0) { fclose(fp); return 0; }

    {   /* Xing / Info：帧数 × 每帧采样 ÷ 采样率 */
        int side = mono ? ((spf == 1152) ? 17 : 9) : ((spf == 1152) ? 32 : 17);
        int xp = p + 4 + side;

        if (xp + 12 <= n && (!memcmp(win + xp, "Xing", 4) || !memcmp(win + xp, "Info", 4))) {
            unsigned flags = ((unsigned)win[xp + 4] << 24) | ((unsigned)win[xp + 5] << 16) |
                             ((unsigned)win[xp + 6] << 8) | win[xp + 7];
            unsigned frames = ((unsigned)win[xp + 8] << 24) | ((unsigned)win[xp + 9] << 16) |
                              ((unsigned)win[xp + 10] << 8) | win[xp + 11];

            if ((flags & 1) && frames > 0) {
                long long ms = (long long)frames * spf * 1000 / sr;
                long long kbps = ms > 0 ? audio * 8000 / ms : 0;

                if (ms >= 3000 && kbps >= 32000 && kbps <= 400000) {
                    if (conf) *conf = 2;
                    fclose(fp);
                    return (int)ms;
                }
            }
        }
    }

    {   /* 没有 Xing：三个位置各取 240 帧，用平均码率估算 */
        for (int w = 0; w < 3; ++w) {
            const unsigned char *b;
            int nn, pp, i;

            if (w == 0) {
                b = win;
                nn = n;
                pp = p;
            } else {
                long long off = (long long)start + audio * (w == 1 ? 5 : 9) / 10;

                if (fseek(fp, (long)off, SEEK_SET) != 0) continue;
                nn = (int)fread(win, 1, sizeof(win), fp);
                pp = sync_frame(win, nn, 4096, &spf, &br, &sr, &pad, &mono);
                if (pp < 0) continue;
                b = win;
            }
            i = pp;
            for (int k = 0; i + 4 <= nn && k < 240; ++k) {
                int L;

                if (!frame_hdr(b, nn, i, &spf, &br, &sr, &pad, &mono)) break;
                L = spf / 8 * br / sr + pad;
                if (L < 24) break;
                bytes += L;
                usum += (long long)spf * 1000000 / sr;        /* 微秒 */
                if (!first_br) first_br = br;
                else if (br != first_br) distinct = 1;
                cnt++;
                i += L;
            }
        }
        fclose(fp);
        if (!cnt || usum <= 0) return 0;
        {
            long long bps = bytes * 1000000 / usum;            /* bytes/s */
            long long ms = bps ? (audio * 1000 / bps) : 0;

            if (ms < 3000 || bps * 8 < 32000 || bps * 8 > 400000) return 0;
            if (conf) *conf = distinct ? 1 : 2;
            return (int)ms;
        }
    }
}

/* 文件名里的网易云 id：MELODY-<id>.mp3 / <id>.mp3 */
long long mp3_id_from_path(const char *path)
{
    const char *b = strrchr(path ? path : "", '/');
    const char *p = b ? b + 1 : (path ? path : "");
    long long id = 0;
    int n = 0;

    if (!strncasecmp(p, "MELODY-", 7)) p += 7;
    while (*p >= '0' && *p <= '9') {
        id = id * 10 + (*p - '0');
        if (++n > 12) return 0;
        p++;
    }
    if (n < 5) return 0;
    if (*p && strcasecmp(p, ".mp3")) return 0;
    return id;
}
