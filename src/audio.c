#include "thread_paths.h"
/*
 * MELODY PSP — 播放核心
 * ------------------------------------------------------------------
 * 两条线程（这次是真·重点）：
 *
 *   decode 线程（PRIO_DEC）：读文件 / 边下边读 → sceMp3 → 重采样 44.1k
 *                            → 写进 2 秒的 PCM 环缓冲
 *   out 线程（PRIO_OUT）  ：只做「从环里搬 OUT_SAMPLES 帧 → 音量
 *                            → sceAudioOutputBlocking」，别的什么都不干
 *
 * 为什么必须拆开（实机测出来的，不是猜的）：
 *   test_audio/AUDIO LAB 在真机 PSP-1000 上的一轮对照：
 *     · 纯 PCM 输出，不跑解码器            → 完全干净
 *     · 解码器 + 输出挤在同一个线程里       → 约 3 次/秒的噼啪，
 *                                            而 under/maxgap 全是 0（硬件从没断粮）
 *     · 解码器挪到另一个线程、中间隔一个环  → 干净
 *     · 前台放 PCM、后台线程单独猛跑解码器  → 干净
 *     · 喂给解码器的块 8KB 换 32KB          → 没区别
 *   结论：sceMp3Decode 和 sceAudioOutputBlocking 不能待在同一个线程里。
 *   拆开以后输出线程每块只花几十微秒，声卡永远有货。
 *
 * 数据来源只有一个：cache/<id>.mp3 这个本地文件。
 *   - 已经在缓存里：直接读文件
 *   - 在线播放：dl 线程往同一个文件里按分片写，decode 线程读「已经连续写好的前缀」
 *
 * seek：out 线程置位 → 等 decode 线程做完（期间出静音）→ decode 线程把环清空、
 *       重建解码器 → 回 ack。快进不再需要重新下载。
 */

#include "audio.h"
#include "skin_sound.h"

#include "app.h"
#include "cache.h"
#include "dl.h"
#include "log.h"

#include <pspkernel.h>
#include <pspaudio.h>
#include <pspiofilemgr.h>
#include <pspmp3.h>
#include <psputility.h>
#include <psputility_avmodules.h>

#include <stdio.h>
#include <string.h>

#define MP3_BUF_SIZE  (40 * 1024)
#define PCM_BUF_SIZE  16384
#define READ_CHUNK    (8 * 1024)
#define OUT_RATE      44100
#define OUT_SAMPLES   1024
#define PREBUFFER     (160 * 1024)
#define STACK_SIZE    0x8000
#define PRIO_OUT      0x14
#define PRIO_DEC      0x15
#define RING_FRAMES   (OUT_RATE * 2)      /* 2 秒 PCM 环 = 345KB */

/* ------------------------------------------------------------------ */
/* 状态（跨线程的都用 volatile）                                       */
/* ------------------------------------------------------------------ */

static volatile int g_gen;
static volatile int g_state;
static volatile int g_pause;
static volatile int g_vol = 80;
static volatile int g_pos_samples;
static volatile int g_seek_ms;
static volatile int g_seek_req;
static volatile int g_seek_seq;
static volatile int g_dur_ms;
static volatile int g_total_bytes;
static volatile int g_audio_start;
static volatile int g_local;
static volatile int g_file_bytes;
static volatile int g_quit;
static volatile int g_kbps;
static volatile int g_rate = OUT_RATE;
static volatile int g_src_rate = 44100;
static volatile int g_channels = 2;
static volatile int g_cached_pct;

static char g_path[128];
static char g_errmsg[96];
static int g_out_thid = -1;
static int g_dec_thid = -1;
static volatile int g_audio_faulted;

/* Single main-thread caller; only output owner touches codec resources. */
static volatile int g_mv_request, g_mv_ack, g_mv_result, g_mv_held;
static volatile int g_mv_restore_pending, g_mv_initial_seek, g_mv_reopening;
static struct {
    int gen, state, paused, pos, local, duration;
    char path[128];
} g_mv_saved;

/* PCM 环：decode 线程只写 w，out 线程只写 r */
static short g_ring[RING_FRAMES * 2] __attribute__((aligned(64)));
static volatile unsigned g_ring_w;
static volatile unsigned g_ring_r;
static volatile int g_dec_quit;
static volatile int g_dec_done;
static volatile int g_dec_ack;
static volatile int g_dec_seek_seen;   /* decode 线程已处理到的 seek 序号 */
static volatile int g_seek_applied;    /* decode 线程实际落到的位置（毫秒） */
static volatile int g_resume_base = -1; /* ≥0：坏块之后从这里（字节）继续 */
static int g_resync_fails;

/* ------------------------------------------------------------------ */
/* 输出侧（只有 out 线程动）                                           */
/* ------------------------------------------------------------------ */

static int g_channel = -1;
static short g_outbuf[OUT_SAMPLES * 2] __attribute__((aligned(64)));

/* 诊断：环饿死的次数 / 最长一次（毫秒） */
static unsigned g_starve;
static int g_starve_max_ms;
static unsigned g_last_pop_us;
static int g_last_state = -1;
static FILE *g_dump_out;
static FILE *g_dump_dec;
static int g_dump_blocks;

/* ------------------------------------------------------------------ */
/* 解码侧（只有 decode 线程动）                                        */
/* ------------------------------------------------------------------ */

static unsigned char g_mp3buf[MP3_BUF_SIZE] __attribute__((aligned(64)));
static unsigned char g_pcmbuf[PCM_BUF_SIZE] __attribute__((aligned(64)));
static short g_stage[OUT_SAMPLES * 2] __attribute__((aligned(64)));
static int g_stage_n;
static int g_res_carry;
static int g_last_sample[2];

static int g_mp3_handle = -1;
static int g_file = -1;
static int g_file_base;
static int g_need_init = 1;
static int g_audio_len;
static long long g_read_pos;
static int g_hdr_rate;
static int g_hdr_ch;
static int g_retry;
static int g_fmt_check;
static int g_feed_empty;
static int g_pending_hint;      /* 解码器缓冲里还没消化的字节数（用来自查当前位置） */
static long long g_last_fix_pos = -1;   /* 上次坏块续播点：保证严格往前走，不来回打转 */
static int g_fix_count;

static long long data_avail(int my, long long off);

/* ------------------------------------------------------------------ */
/* 日志 / 状态                                                         */
/* ------------------------------------------------------------------ */

static void set_state(int st, const char *why)
{
    int old;

    if (st == g_last_state) return;
    old = g_last_state;
    g_last_state = st;
    mlog("audio state %d->%d (%s) pos=%dms", old, st, why ? why : "", audio_pos_ms());
}

void audio_stall_stats(int *count, int *max_ms);

void audio_debug_dump(int blocks)
{
    if (g_dump_out) { fclose(g_dump_out); g_dump_out = NULL; }
    if (g_dump_dec) { fclose(g_dump_dec); g_dump_dec = NULL; }
    g_dump_blocks = blocks;
    g_dump_out = fopen("./pcm_out.raw", "wb");
    g_dump_dec = fopen("./pcm_dec.raw", "wb");
    mlog("audio dump: %d blocks, files %p/%p", blocks, g_dump_out, g_dump_dec);
}

void audio_stall_stats(int *count, int *max_ms)
{
    if (count) *count = (int)g_starve;
    if (max_ms) *max_ms = g_starve_max_ms;
}

/* ------------------------------------------------------------------ */
/* PCM 环                                                              */
/* ------------------------------------------------------------------ */

/* 只由 decode 线程调用 */
static void ring_push(const short *b, int n)
{
    int guard = 0;
    unsigned idx;
    int first;

    if (n <= 0) return;
    if (n > RING_FRAMES) n = RING_FRAMES;

    while (!g_quit && !g_dec_quit &&
           g_ring_w - g_ring_r + (unsigned)n > RING_FRAMES) {
        /* 等环的时候必须盯着 seek：否则输出端在等 ack、这里在等空位 = 死锁 */
        if (g_seek_seq != g_dec_seek_seen) return;
        if (++guard > 6000) return;
        sceKernelDelayThread(2000);
    }

    idx = g_ring_w % RING_FRAMES;
    first = RING_FRAMES - (int)idx;
    if (first > n) first = n;
    memcpy(g_ring + idx * 2, b, (size_t)first * 4);
    if (n > first) memcpy(g_ring, b + first * 2, (size_t)(n - first) * 4);
    g_ring_w += (unsigned)n;
}

/* 只由 out 线程调用；取到一块返回 1 */
static int ring_pop(short *b, int n)
{
    unsigned idx;
    int first;

    if (g_ring_w - g_ring_r < (unsigned)n) return 0;
    idx = g_ring_r % RING_FRAMES;
    first = RING_FRAMES - (int)idx;
    if (first > n) first = n;
    memcpy(b, g_ring + idx * 2, (size_t)first * 4);
    if (n > first) memcpy(b + first * 2, g_ring, (size_t)(n - first) * 4);
    g_ring_r += (unsigned)n;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 重采样到 44.1kHz（decode 线程）                                     */
/* ------------------------------------------------------------------ */

static void stage_flush(void);

static void reset_resampler(void)
{
    g_stage_n = 0;
    g_res_carry = 0;
    g_last_sample[0] = g_last_sample[1] = 0;
}

static void decode_output(short *in, int in_n, int ch, int rate)
{
    int step;
    int pos;

    if (in_n <= 0) return;

    if (g_dump_dec && g_dump_blocks > 0) {
        fwrite(in, 2, (size_t)(in_n * ch), g_dump_dec);
        fflush(g_dump_dec);
    }

    if (ch < 1) ch = 1;
    if (ch > 2) ch = 2;
    if (rate <= 0) rate = 44100;

    /* 必须 64 位：rate<<16 对 44100/48000 会溢出 int32 */
    step = (int)(((long long)rate << 16) / OUT_RATE);
    pos = g_res_carry;

    while ((pos >> 16) < in_n) {
        int v = pos >> 16;
        int frac = pos & 0xFFFF;
        int a0, a1, b0, b1, l, r;

        a0 = (v == 0) ? g_last_sample[0] : in[(v - 1) * ch];
        b0 = in[v * ch];
        l = a0 + (int)(((long long)(b0 - a0) * frac) >> 16);

        if (ch == 2) {
            a1 = (v == 0) ? g_last_sample[1] : in[(v - 1) * ch + 1];
            b1 = in[v * ch + 1];
            r = a1 + (int)(((long long)(b1 - a1) * frac) >> 16);
        } else {
            r = l;
        }

        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;

        g_stage[g_stage_n * 2] = (short)l;
        g_stage[g_stage_n * 2 + 1] = (short)r;
        if (++g_stage_n >= OUT_SAMPLES) stage_flush();

        pos += step;
    }

    g_res_carry = pos - (in_n << 16);
    if (g_res_carry < 0) g_res_carry = 0;

    g_last_sample[0] = in[(in_n - 1) * ch];
    g_last_sample[1] = (ch == 2) ? in[(in_n - 1) * ch + 1] : g_last_sample[0];
}

static void stage_flush(void)
{
    if (g_stage_n <= 0) return;
    ring_push(g_stage, g_stage_n);
    g_stage_n = 0;
}

/* ------------------------------------------------------------------ */
/* MP3 帧头                                                            */
/* ------------------------------------------------------------------ */

static const int g_bitrate1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
static const int g_bitrate2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
static const int g_srate1[4] = { 44100, 48000, 32000, 0 };
static const int g_srate2[4] = { 22050, 24000, 16000, 0 };
static const int g_srate25[4] = { 11025, 12000, 8000, 0 };

static int mp3_frame_len(const unsigned char *b, int *sr_out, int *br_out, int *ch_out)
{
    int ver, layer, br_idx, sr_idx, pad, br, sr, len, ch;

    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return 0;

    ver = (b[1] >> 3) & 3;
    layer = (b[1] >> 1) & 3;
    br_idx = (b[2] >> 4) & 15;
    sr_idx = (b[2] >> 2) & 3;
    pad = (b[2] >> 1) & 1;

    if (ver == 1 || layer != 1) return 0;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return 0;

    br = (ver == 3) ? g_bitrate1[br_idx] : g_bitrate2[br_idx];
    sr = (ver == 3) ? g_srate1[sr_idx] : ((ver == 2) ? g_srate2[sr_idx] : g_srate25[sr_idx]);
    if (br <= 0 || sr <= 0) return 0;

    if (ver == 3) len = 144 * br * 1000 / sr + pad;
    else len = 72 * br * 1000 / sr + pad;

    ch = (b[3] >> 6) & 3;
    if (ch_out) *ch_out = (ch == 3) ? 1 : 2;
    if (sr_out) *sr_out = sr;
    if (br_out) *br_out = br;
    return (len > 0 && len < 8192) ? len : 0;
}

static int mp3_find_frame(const unsigned char *buf, int len, int *sr_out, int *br_out, int *ch_out)
{
    for (int i = 0; i + 4 <= len; ++i) {
        int fl, sr = 0, br = 0, ch = 0;

        if (buf[i] != 0xFF) continue;
        fl = mp3_frame_len(buf + i, &sr, &br, &ch);
        if (!fl) continue;
        if (i + fl + 4 <= len) {
            int fl2 = mp3_frame_len(buf + i + fl, NULL, NULL, NULL);

            if (!fl2) continue;
        }
        if (sr_out) *sr_out = sr;
        if (br_out) *br_out = br;
        if (ch_out) *ch_out = ch;
        return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* 数据可用性                                                          */
/* ------------------------------------------------------------------ */

static long long data_avail(int my, long long off)
{
    long long end;

    if (g_local) end = g_file_bytes;
    else end = dl_stream_available(my);
    return end > off ? end - off : 0;
}

static int stream_finished(int my)
{
    if (g_local) return 1;
    return dl_stream_done(my) ? 1 : 0;
}

/* 等 off 处至少 need 字节；1 够 / 0 结束但不够 / -1 中断 / -2 失败 */
static int wait_data(int my, long long off, int need)
{
    int tick = 0;

    /* 不能设"总时长上限"：边下边播时下载慢一点的歌会被判死，
     * 结果就是「缓存完了也不播」。只由换歌/退出/失败来决定结束。 */
    for (;;) {
        long long have;

        if (g_quit || g_dec_quit || g_gen != my || g_seek_seq!=g_dec_seek_seen) return -1;
        if(!g_local)dl_stream_readable(my,off,need);
        have = data_avail(my, off);
        if (have >= need) return 1;

        if (!g_local && dl_stream_failed(my)) {
            snprintf(g_errmsg, sizeof(g_errmsg), "%s",
                     dl_stream_error()[0] ? dl_stream_error() : "下载中断");
            return -2;
        }

        if (stream_finished(my)) return have > 0 ? 0 : -2;

        if ((++tick % 400) == 0)
            mlog("dec wait data @%lld need=%d have=%lld", off, need, have);

        sceKernelDelayThread(15 * 1000);
    }
}

/* ------------------------------------------------------------------ */
/* 解码器                                                              */
/* ------------------------------------------------------------------ */

static void close_mp3(void)
{
    if (g_mp3_handle >= 0) {
        sceMp3ReleaseMp3Handle(g_mp3_handle);
        g_mp3_handle = -1;
    }
}

/* 读播放流文件。解码线程的 fd 可能比 dl 线程的 open(O_TRUNC) 早，或者
 * 真机 FAT 把文件大小缓存在 fd/目录项里；短读时同步 + 重开一次刷新目录项。
 * pfd 指向 g_file，重开后自动更新，后续调用继续用新 fd。 */
static int stream_read(int *pfd, long long off, void *buf, int n)
{
    int fd = *pfd;
    int got;

    if (fd < 0) return -1;
    if(!g_local){int readable=dl_stream_readable(g_gen,off,n);if(readable<=0)return 0;if(n>readable)n=readable;}
    if (sceIoLseek(fd, (SceOff)off, PSP_SEEK_SET) < 0) return -1;
    got = sceIoRead(fd, buf, (SceSize)n);
    if (got < n && !g_local) {
        int nfd;

        sceIoSync("ms0:", 0);
        nfd = sceIoOpen(g_path, PSP_O_RDONLY, 0);
        if (nfd >= 0) {
            sceIoClose(fd);
            *pfd = nfd;
            fd = nfd;
            if (sceIoLseek(fd, (SceOff)off, PSP_SEEK_SET) >= 0)
                got = sceIoRead(fd, buf, (SceSize)n);
            mlog("stream reopen @%lld got=%d/%d", off, got, n);
        }
    }
    return got;
}

static int feed_once(int fd, int my)
{
    SceUChar8 *dst = NULL;
    SceInt32 towrite = 0, pos = 0;
    int want, got;
    long long off;

    if (fd < 0) return -1;
    if (sceMp3GetInfoToAddStreamData(g_mp3_handle, &dst, &towrite, &pos) < 0) return -1;
    if (towrite > 0)
        g_pending_hint = MP3_BUF_SIZE - 1472 - (int)towrite;    /* 还没解码的字节 */
    if (towrite <= 0) return 0;

    want = towrite > READ_CHUNK ? READ_CHUNK : towrite;
    off = (long long)g_file_base + pos;
    g_read_pos = off;

    {
        int r = wait_data(my, off, want);

        if (r == -1) return -1;
        if (r == -2) return -2;
        if (r == 0) {
            long long have = data_avail(my, off);

            if (have <= 0) return -2;
            want = (int)have;
            if (want > towrite) want = towrite;
        }
    }

    got = stream_read(&g_file, off, dst, want);
    if (got <= 0) return -2;

    if (sceMp3NotifyAddStreamData(g_mp3_handle, got) < 0) return -1;
    return got;
}

static int find_frame_at(int fd, int my, long long off, int *frame_len)
{
    static unsigned char buf[8192];
    int got, idx, sr = 0, br = 0, ch = 0;

    if (wait_data(my, off, 2048) < 0) return -1;

    got = stream_read(&g_file, off, buf, sizeof(buf));
    if (got < 4) return -1;

    idx = mp3_find_frame(buf, got, &sr, &br, &ch);
    if (idx < 0) return -1;

    if (frame_len) *frame_len = mp3_frame_len(buf + idx, NULL, NULL, NULL);
    g_hdr_rate = sr;
    g_hdr_ch = ch;
    return (int)(off + idx);
}

/* 从 start 起找一段「连续 nframes 帧都合法、且采样率/声道一致」的位置。
 * 用连续多帧而不是一两帧来判断，是为了避开坏块里偶然出现的假同步字节
 * —— 以前就是撞上假同步，解出来是噪音，又报错，又续到同一处，来回打转。 */
static int find_clean_run(int fd, int my, long long start, int nframes)
{
    static unsigned char buf[16384];
    long long base = start;
    int guard = 0;

    while (guard++ < 32) {
        int got;

        if (g_quit || g_dec_quit || g_gen != my) return -1;
        if (g_file_bytes > 0 && base >= g_file_bytes - 1024) return -1;

        if (wait_data(my, base, 2048) < 0) {
            if (g_local) {
                if (base >= g_file_bytes - 1024) return -1;
            } else {
                return -1;
            }
        }
        got = stream_read(&g_file, base, buf, sizeof(buf));
        if (got < 128) return -1;

        for (int i = 0; i + 4 <= got; ++i) {
            int sr0 = 0, ch0 = 0, n = 0, o;

            if (buf[i] != 0xFF) continue;
            if (!mp3_frame_len(buf + i, &sr0, NULL, &ch0)) continue;

            o = i;
            while (n < nframes) {
                int sr2 = 0, ch2 = 0, f2;

                if (o + 4 > got) { n = -1; break; }
                f2 = mp3_frame_len(buf + o, &sr2, NULL, &ch2);
                if (!f2 || sr2 != sr0 || ch2 != ch0) { n = -1; break; }
                o += f2;
                n++;
            }
            if (n == nframes) return (int)(base + i);
        }
        base += got - 4096;
    }
    return -1;
}
static int open_mp3_at(int fd, int my)
{
    SceMp3InitArg arg;
    int st, tries;
    int frame_len = 0;
    int start;
    int sr = 0;

    start = find_frame_at(fd, my, g_file_base, &frame_len);
    if (start < 0) {
        mlog("no frame sync at %d", g_file_base);
        return -1;
    }
    if (start != g_file_base) {
        mlog("sync +%d bytes (base %d)", start - g_file_base, g_file_base);
        g_file_base = start;
    }

    memset(&arg, 0, sizeof(arg));
    arg.mp3StreamStart = 0;
    /* 必须 0x7FFFFFFF：给真实长度时 sceMp3 对 48kHz 的 320k 流会汇报成 24kHz */
    arg.mp3StreamEnd = 0x7FFFFFFF;
    arg.mp3Buf = g_mp3buf;
    arg.mp3BufSize = sizeof(g_mp3buf);
    arg.pcmBuf = g_pcmbuf;
    arg.pcmBufSize = sizeof(g_pcmbuf);

    for (tries = 0; tries < 6; ++tries) {
        g_mp3_handle = sceMp3ReserveMp3Handle(&arg);
        if (g_mp3_handle < 0) {
            mlog("ReserveMp3Handle = 0x%08X", g_mp3_handle);
            g_mp3_handle = -1;
            return -2;
        }

        for (int i = 0; i < 4; ++i) {
            int n = feed_once(fd, my);

            if (n <= 0) break;
        }

        st = sceMp3Init(g_mp3_handle);
        if (st >= 0) break;

        mlog("sceMp3Init = 0x%08X at %d, retry", st, g_file_base);
        sceMp3ReleaseMp3Handle(g_mp3_handle);
        g_mp3_handle = -1;

        if (frame_len <= 0) frame_len = 417;
        g_file_base += frame_len;
        start = find_frame_at(fd, my, g_file_base, &frame_len);
        if (start < 0) return -2;
        g_file_base = start;
    }

    if (g_mp3_handle < 0) return -2;

    g_src_rate = sceMp3GetSamplingRate(g_mp3_handle);
    g_channels = sceMp3GetMp3ChannelNum(g_mp3_handle);

    if (g_hdr_ch > 0 && g_hdr_ch != g_channels) {
        mlog("channels: decoder=%d header=%d -> use header", g_channels, g_hdr_ch);
        g_channels = g_hdr_ch;
    }
    g_kbps = sceMp3GetBitRate(g_mp3_handle);
    sr = g_hdr_rate;

    if (sr > 0) g_src_rate = sr;
    if (g_src_rate <= 0) g_src_rate = 44100;
    if (g_channels <= 0) g_channels = 2;
    if (g_kbps <= 0) g_kbps = 128;

    if (g_dur_ms <= 0 && g_audio_len > 0 && g_kbps > 0) {
        g_dur_ms = (int)((long long)g_audio_len * 8 / g_kbps);
        mlog("dur estimate %dms (len %d, %dkbps)", g_dur_ms, g_audio_len, g_kbps);
    }

    mlog("mp3 hdr=%dHz dec=%dHz %dch hdrch=%d %dkbps base=%d len=%d local=%d",
         sr, (int)g_src_rate, g_channels, g_hdr_ch, g_kbps, g_file_base, g_audio_len,
         (int)g_local);
    return 0;
}

static int detect_id3(int fd, int my)
{
    unsigned char hdr[10];
    int got;
    int total;

    g_audio_start = 0;
    total = g_local ? g_file_bytes : dl_stream_total(my);
    g_total_bytes = total;

    if (wait_data(my, 0, 10) < 0) return -1;

    got = stream_read(&g_file, 0, hdr, sizeof(hdr));
    if (got < 10) {
        mlog("id3 read got=%d", got);
        return -1;
    }

    if (!memcmp(hdr, "ID3", 3)) {
        int sz = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                 ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
        int start = sz + 10 + ((hdr[5] & 0x10) ? 10 : 0);

        if (start > 0 && start < 1024 * 1024) {
            g_audio_start = start;
            mlog("id3 skip %d", start);
        }
    }

    g_audio_len = (total > 0) ? total - g_audio_start : 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* decode 线程                                                         */
/* ------------------------------------------------------------------ */

static int decode_thread(SceSize args, void *argp)
{
#if defined(__PSP__)
    melody_bind_thread_directory();
#endif

    int my = g_gen;
    int local_seek = g_seek_seq - (g_mv_initial_seek ? 1 : 0);
    int init_done = 0;

    g_mv_initial_seek = 0;
    g_dec_seek_seen = local_seek;

    (void)args;
    (void)argp;

    /* An interrupted replay can leave an existing empty/partial file. On PSP,
     * opening its reader before dl's O_TRUNC can deny the writer (0x8001000D).
     * Wait for this generation's published bytes before taking a read handle;
     * this also waits through checkpoint validation and file recreation. */
    if (!g_local) {
        for (;;) {
            int ready = wait_data(my, 0, 1);
            if (ready >= 0) break;
            if (ready == -1 && !g_quit && !g_dec_quit && g_gen == my &&
                g_seek_seq != g_dec_seek_seen) {
                /* Keep local_seek unchanged: the decode loop applies this seek. */
                g_dec_seek_seen = g_seek_seq;
                continue;
            }
            if (ready == -2) g_state = AUDIO_ERROR;
            return 0;
        }
    }

    /* dl 线程是先起 gen、后建文件的，这里可能一时还打不开 —— 重试，别直接判死 */
    for (int i = 0; i < 250; ++i) {
        g_file = sceIoOpen(g_path, PSP_O_RDONLY, 0777);
        if (g_file >= 0) break;
        if (i == 0) mlog("audio open %s -> %d, retry", g_path, g_file);
        if (g_quit || g_dec_quit || g_gen != my) break;
        sceKernelDelayThread(20 * 1000);
    }
    mlog("audio open %s -> %d", g_path, g_file);
    if (g_file < 0) {
        g_state = AUDIO_ERROR;
        snprintf(g_errmsg, sizeof(g_errmsg), "文件打不开");
        return 0;
    }

    for (;;) {
        if (g_quit || g_dec_quit || g_gen != my) break;

        /* 新的 seek：丢弃环里旧数据，重建解码器 */
        if (g_seek_seq != local_seek) {
            local_seek = g_seek_seq;
            g_dec_seek_seen = local_seek;
            close_mp3();
            g_need_init = 1;
            init_done = 0;
            g_dec_done = 0;
            g_seek_applied = g_seek_req;
            g_resume_base = -1;
            g_last_fix_pos = -1;
            g_fix_count = 0;
            g_resync_fails = 0;
            g_ring_r = g_ring_w;          /* 丢掉环里的旧数据（只动 r，让输出端自己看到空） */
            reset_resampler();
            g_dec_ack = local_seek;
            mlog("dec seek -> %dms", g_seek_req);
        }

        if (!init_done) {
            long long target;
            int dur = g_dur_ms;
            int alen;
            int resuming = 0;

            {
                int r = wait_data(my, 0, PREBUFFER);

                if(r==-1){if(!g_quit&&!g_dec_quit&&g_gen==my&&g_seek_seq!=local_seek)continue;break;}
                if (r == -2) {
                    g_state = AUDIO_ERROR;
                    if (!g_errmsg[0]) snprintf(g_errmsg, sizeof(g_errmsg), "没有数据");
                    sceKernelDelayThread(100 * 1000);
                    continue;
                }
            }

            if (detect_id3(g_file, my) < 0) {
                sceKernelDelayThread(20 * 1000);
                continue;
            }

            alen = g_audio_len;
            if (g_resume_base >= 0) {
                /* 坏块/坏帧之后：从这个字节继续，绝不能从头重放 */
                target = g_resume_base;
                g_resume_base = -1;
                resuming = 1;
                mlog("resume at byte %d", (int)target);
            } else {
                target = g_audio_start;
                if (g_seek_applied > 0 && alen > 0 && dur > 0) {
                    target += (long long)alen * g_seek_applied / dur;
                    if (target >= g_audio_start + alen - 1024)
                        target = g_audio_start + alen - 1024;
                }
            }
            g_file_base = (int)target;

            {
                int r = wait_data(my, g_file_base, 4096);

                if(r==-1){if(!g_quit&&!g_dec_quit&&g_gen==my&&g_seek_seq!=local_seek)continue;break;}
                if (r == -2) {
                    g_state = AUDIO_ERROR;
                    snprintf(g_errmsg, sizeof(g_errmsg), "下载中断");
                    sceKernelDelayThread(100 * 1000);
                    continue;
                }
            }

            if (open_mp3_at(g_file, my) < 0) {
                if(g_seek_seq!=local_seek&&!g_quit&&!g_dec_quit&&g_gen==my)continue;
                if (g_mv_reopening) {
                    mlog("audio mv: restored MP3 reserve/init failed; retaining fault");
                    g_audio_faulted = 1;
                    g_state = AUDIO_ERROR;
                    break;
                }
                if (resuming) {
                    /* 续播点还是坏的：继续往前跳，绝不回到开头 */
                    int nb = g_file_base + 8 * 1024;

                    if (++g_resync_fails > 8 || nb >= g_file_bytes - 4096) {
                        mlog("resume gave up at %d", g_file_base);
                        g_state = AUDIO_ERROR;
                        snprintf(g_errmsg, sizeof(g_errmsg), "文件损坏");
                        g_dec_done=1;
                        break;
                    }
                    g_resume_base = nb;
                    mlog("resume retry -> %d", nb);
                    sceKernelDelayThread(20 * 1000);
                    continue;
                }
                if (g_file_base > g_audio_start) {
                    mlog("seek init failed, fallback to start");
                    g_file_base = g_audio_start;
                    g_seek_applied = 0;
                    if (open_mp3_at(g_file, my) < 0) {
                        g_state = AUDIO_ERROR;
                        snprintf(g_errmsg, sizeof(g_errmsg), "解码器初始化失败");
                        sceKernelDelayThread(120 * 1000);
                        continue;
                    }
                } else {
                    g_state = AUDIO_ERROR;
                    snprintf(g_errmsg, sizeof(g_errmsg), "解码器初始化失败");
                    sceKernelDelayThread(120 * 1000);
                    continue;
                }
            }

            init_done = 1;
            g_need_init = 0;
            g_resync_fails = 0;
            g_read_pos = g_file_base;
            g_fmt_check = 0;
            g_feed_empty = 0;
            reset_resampler();
            mlog("audio playing from %dms (byte %d) resume=%d", (int)g_seek_applied,
                 g_file_base, resuming);
        }

        if (g_mp3_handle < 0) {
            init_done = 0;
            continue;
        }

        if (g_dur_ms > 0 && audio_pos_ms() > g_dur_ms + 2000) {
            g_dec_done = 1;
            set_state(AUDIO_END, "past dur");
            break;
        }

        if (sceMp3CheckStreamDataNeeded(g_mp3_handle) > 0) {
            int fed = feed_once(g_file, my);

            if (fed == -1) {
                if (g_gen != my) break;
            } else if (fed == -2) {
                if (g_gen != my) break;
                if (g_feed_empty < 5) {
                    g_feed_empty++;
                    mlog("audio feed end: local=%d avail=%lld done=%d failed=%d",
                         (int)g_local,
                         (long long)(g_local ? g_file_bytes - g_file_base
                                             : dl_stream_available(my)),
                         (int)stream_finished(my), (int)(!g_local && dl_stream_failed(my)));
                }
                if (!g_local && dl_stream_failed(my)) {
                    if (++g_retry > 3) {
                        g_state = AUDIO_ERROR;
                        snprintf(g_errmsg, sizeof(g_errmsg), "下载中断");
                        g_retry = 0;
                        sceKernelDelayThread(100 * 1000);
                    } else {
                        sceKernelDelayThread(200 * 1000);
                    }
                    continue;
                }
            } else if (fed > 0) {
                g_retry = 0;
            }
        }

        {
            short *pcm = NULL;
            int bytes = sceMp3Decode(g_mp3_handle, &pcm);

            if (bytes > 0) {
                g_mv_reopening = 0;
                if (g_fmt_check < 3) {
                    int ns = bytes / (2 * (g_channels > 0 ? g_channels : 2));

                    if (ns != 1152 && ns != 576) {
                        mlog("decode frame size odd: bytes=%d ch=%d -> %d samples", bytes,
                             g_channels, ns);
                        g_fmt_check = 3;
                    } else if (g_fmt_check == 0) {
                        g_fmt_check = 1;
                        mlog("decode ok: %d samples/frame ch=%d", ns, g_channels);
                    }
                }
                decode_output(pcm, bytes / (2 * g_channels), g_channels, g_src_rate);
                continue;
            }

            if (bytes == 0 || (unsigned int)bytes == 0x80671402u) {
                /* g_audio_len excludes ID3; seek/resync moves g_file_base,
                 * but must not move the absolute end of the source file. */
                int at_end = (g_audio_len > 0) &&
                             (g_read_pos >= (long long)g_audio_start + g_audio_len - 4096);

                if (stream_finished(my) && at_end) {
                    stage_flush();
                    g_dec_done = 1;
                    /* PCM may still contain seconds of sound. Only the output
                     * thread announces END after draining the ring. */
                    break;
                }
                sceKernelDelayThread(5 * 1000);
                continue;
            }

            mlog("sceMp3Decode = 0x%08X base=%d pos=%dms", bytes, g_file_base, audio_pos_ms());
            if (g_mv_reopening) {
                mlog("audio mv: first decode failed 0x%08X; no resync/restart", bytes);
                g_audio_faulted = 1;
                g_state = AUDIO_ERROR;
                break;
            }

            {
                long long dpos, from;
                int nxt;

                /* 解码位置 = 已经喂到哪儿 - 缓冲里还没消化的字节。
                 * 再往 +128 字节挪一点，避免正好停在坏帧头上；同时保证
                 * 比上一次续播点至少再前进 512 字节（否则会原地打转、
                 * 反复播同一段损坏的音频）。 */
                dpos = (long long)g_read_pos - g_pending_hint;
                if (g_pending_hint <= 0) dpos = (long long)g_read_pos - 16 * 1024;
                if (dpos < (long long)g_audio_start) dpos = g_audio_start;
                if (g_read_pos <= g_audio_start) dpos = g_audio_start;

                from = dpos + 128;
                if (g_last_fix_pos > 0 && from < g_last_fix_pos + 512)
                    from = g_last_fix_pos + 512;

                mlog("resync: fed=%lld pending=%d decode=%lld from=%lld",
                     (long long)g_read_pos, g_pending_hint, dpos, from);

                nxt = find_clean_run(g_file, my, from, 8);
                if (nxt < 0) nxt = find_clean_run(g_file, my, from + 32 * 1024, 8);
                if (nxt < 0) nxt = find_clean_run(g_file, my, from + 256 * 1024, 24);

                g_fix_count++;
                if(g_fix_count>8){
                    mlog("audio resync limit reached; preserve source, require explicit retry/download");
                    snprintf(g_errmsg,sizeof(g_errmsg),"缓存音频损坏，请重新下载");
                    g_state=AUDIO_ERROR;g_dec_done=1;break;
                }
                close_mp3();
                init_done = 0;
                if (nxt > 0 && (g_last_fix_pos < 0 || nxt > g_last_fix_pos)) {
                    g_resume_base = nxt;
                    g_last_fix_pos = nxt;
                    mlog("resync ok -> %d (fix #%d)", nxt, g_fix_count);
                    snprintf(g_errmsg, sizeof(g_errmsg), "重新同步");
                } else {
                    snprintf(g_errmsg, sizeof(g_errmsg), "解码失败 (0x%08X)", bytes);
                    g_state = AUDIO_ERROR;
                    sceKernelDelayThread(100 * 1000);
                }
            }
        }
    }

    stage_flush();
    close_mp3();
    if (g_file >= 0) {
        sceIoClose(g_file);
        g_file = -1;
    }
    return 0;
}

static int retire_decode_thread(void)
{
    SceUInt timeout = 500000;
    int r;
    if (g_dec_thid < 0) return 0;
    g_dec_quit = 1;
    r = sceKernelWaitThreadEnd(g_dec_thid, &timeout);
    if (r >= 0) r = sceKernelDeleteThread(g_dec_thid);
    if (r < 0) {
        g_audio_faulted = 1;
        g_state = AUDIO_ERROR;
        snprintf(g_errmsg, sizeof(g_errmsg), "音频未能停止，请重启");
        mlog("audio: worker retained after join failure 0x%08X", r);
        return r;
    }
    g_dec_thid = -1;
    g_dec_quit = 0;
    return 0;
}

/* MV_HANDOFF_BEGIN: also extracted by host regression tests. */
static int audio_mv_owner_request(void)
{
    int request = g_mv_request;
    int r = 0;
    if (!request || g_mv_ack) return g_mv_held;
    if (g_audio_faulted) r = -1;
    else if (request == 1) {
        g_mv_saved.gen = g_gen;
        g_mv_saved.state = g_state;
        g_mv_saved.paused = g_pause;
        g_mv_saved.pos = audio_pos_ms();
        g_mv_saved.local = g_local;
        g_mv_saved.duration = g_dur_ms;
        snprintf(g_mv_saved.path, sizeof(g_mv_saved.path), "%s", g_path);
        g_mv_held = 1;
        r = retire_decode_thread();
        if (r >= 0) {
            close_mp3();
            if (g_file >= 0) {
                r = sceIoClose(g_file);
                if (r >= 0) g_file = -1;
            }
            if (r >= 0 && g_channel >= 0) {
                r = sceAudioChRelease(g_channel);
                if (r >= 0) g_channel = -1;
            }
            if (r >= 0) r = sceMp3TermResource();
            mlog("audio mv: retired gen=%d pos=%d pause=%d term=0x%08X",
                 g_mv_saved.gen, g_mv_saved.pos, g_mv_saved.paused, r);
        }
    } else if (request == 2 && g_mv_held) {
        /* Called only after MV worker/ME stop. No handle exists here. */
        if (g_dec_thid >= 0 || g_mp3_handle >= 0 || g_file >= 0 || g_channel >= 0)
            r = -1;
        else {
            r = sceMp3InitResource();
            mlog("audio mv: init resource=0x%08X gen=%d", r, (int)g_gen);
            if (r >= 0 && !g_audio_faulted) {
                g_mv_restore_pending = g_mv_saved.gen == g_gen &&
                    !strcmp(g_mv_saved.path, g_path) && g_path[0] &&
                    g_mv_saved.state != AUDIO_IDLE && g_state != AUDIO_IDLE;
                if (g_mv_restore_pending) {
                    g_local = g_mv_saved.local;
                    g_dur_ms = g_mv_saved.duration;
                    g_pause = g_mv_saved.paused;
                    g_state = AUDIO_BUFFER;
                }
                g_mv_held = 0;
            } else if (r >= 0) r = -1; /* caller timed out during init */
        }
    } else r = -1;
    if (r < 0) {
        g_audio_faulted = 1;
        g_state = AUDIO_ERROR;
        mlog("audio mv: handoff failed request=%d r=0x%08X", request, r);
    }
    g_mv_result = r;
    __sync_synchronize();
    g_mv_ack = request;
    return g_mv_held;
}

static int audio_mv_submit(int request)
{
    unsigned start;
    int r;
    if (g_audio_faulted || g_quit || g_out_thid < 0 || g_mv_request) return -1;
    if (request == 1 && g_mv_held) return -1;
    if (request == 2 && !g_mv_held) return 0;
    g_mv_ack = 0;
    g_mv_result = -1;
    __sync_synchronize();
    g_mv_request = request;
    start = sceKernelGetSystemTimeLow();
    while (g_mv_ack != request) {
        if ((unsigned)(sceKernelGetSystemTimeLow() - start) >= 2000000u) {
            g_audio_faulted = 1;
            g_state = AUDIO_ERROR;
            mlog("audio mv: owner ack timeout request=%d", request);
            /* Retain pending request; late owner ack must never allow replay. */
            return -1;
        }
        sceKernelDelayThread(1000);
    }
    __sync_synchronize();
    r = g_mv_result;
    g_mv_request = 0;
    g_mv_ack = 0;
    return g_audio_faulted ? -1 : r;
}

int audio_mv_suspend(void) { return audio_mv_submit(1); }
int audio_mv_resume(void) { return audio_mv_submit(2); }
/* MV_HANDOFF_END */

/* ------------------------------------------------------------------ */
/* out 线程：只搬数据 + 送声卡                                         */
/* ------------------------------------------------------------------ */

/* One diagnostic per accepted voice, at the actual hardware submission. */
static volatile int g_music_level;
static int audio_measure_level(const short*pcm,int frames,int channels){
 if(!pcm||frames<1||channels<1)return 0;
 unsigned sum=0,count=0;for(int frame=0;frame<frames;frame+=16)for(int channel=0;channel<channels;++channel){int v=pcm[frame*channels+channel];if(v<0)v=-v;sum+=(unsigned)v;count++;}
 if(!count)return 0;
 unsigned level=(sum/count)*100u/16384u;return level>100?100:(int)level;
}
int audio_level(void){return !g_pause&&g_state==AUDIO_PLAY&&!g_audio_faulted&&!g_mv_held&&!g_mv_request?g_music_level:0;}

static void audio_skin_output(void)
{
    static unsigned reported;
    unsigned plays = skin_sound_play_count();
    int result = sceAudioOutputBlocking(g_channel, PSP_AUDIO_VOLUME_MAX, g_outbuf);
        if (g_dump_out && g_dump_blocks > 0) {
            fwrite(g_outbuf, 2, (size_t)(OUT_SAMPLES * 2), g_dump_out);
            fflush(g_dump_out);
            if (--g_dump_blocks == 0) {
                fclose(g_dump_out);
                g_dump_out = NULL;
                if (g_dump_dec) {
                    fclose(g_dump_dec);
                    g_dump_dec = NULL;
                }
                mlog("audio dump done");
            }
        }

    if (plays != reported && skin_sound_mixed_frames()) {
        mlog("skin sound output channel=%d result=%d plays=%u mixed=%u peak=%d",
             g_channel, result, plays, skin_sound_mixed_frames(), skin_sound_peak());
        reported = plays;
    }
}

/* Same output owner/channel for UI-only audio: never reserve during MV.
 * Returns 1 when it submitted a block, otherwise caller retains idle pacing. */
static int audio_skin_silence(void)
{
    if (g_quit || g_audio_faulted || g_mv_held || g_mv_request || !skin_sound_active()) return 0;
    if (g_channel < 0) {
        g_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OUT_SAMPLES,
                                     PSP_AUDIO_FORMAT_STEREO);
        if (g_channel < 0) { g_channel = -1; skin_sound_stop(); return 0; }
    }
    memset(g_outbuf, 0, sizeof(g_outbuf));
    skin_sound_mix(g_outbuf, OUT_SAMPLES, 2, OUT_RATE);
    if (g_quit || g_audio_faulted || g_mv_request || g_mv_held) return 0;
    audio_skin_output();
    return 1;
}

static int audio_thread(SceSize args, void *argp)
{
#if defined(__PSP__)
    melody_bind_thread_directory();
#endif

    int local_gen = -1;
    int local_ack = -1;
    unsigned hb_ms = 0;

    (void)args;
    (void)argp;

    for (;;) {
        int my;
        int avail;

        if (g_quit) break;
        if (g_audio_faulted) {
            g_state = AUDIO_ERROR;
            sceKernelDelayThread(50000);
            continue;
        }
        if (g_mv_request || g_mv_held) skin_sound_stop();
        if (audio_mv_owner_request()) {
            local_gen = -1;
            sceKernelDelayThread(1000);
            continue;
        }
        if (g_audio_faulted) continue;
        my = g_gen;

        if (g_state == AUDIO_IDLE) {
            if (retire_decode_thread() < 0) continue;
            if (g_file >= 0) {
                sceIoClose(g_file);
                g_file = -1;
            }
            close_mp3();
            local_gen = -1;
            if (!audio_skin_silence()) sceKernelDelayThread(25 * 1000);
            continue;
        }

        /* 换歌 */
        if (my != local_gen) {
            if (retire_decode_thread() < 0) continue;
            close_mp3();
            if (g_file >= 0) {
                sceIoClose(g_file);
                g_file = -1;
            }

            local_gen = my;
            g_pos_samples = 0;
            g_seek_ms = g_mv_restore_pending ? g_mv_saved.pos : 0;
            g_file_base = 0;
            g_audio_start = 0;
            g_audio_len = 0;
            g_hdr_rate = 0;
            g_hdr_ch = 0;
            g_retry = 0;
            g_feed_empty = 0;
            g_dec_done = 0;
            g_ring_w = 0;
            g_ring_r = 0;
            g_seek_applied = 0;
            g_resume_base = -1;
            g_last_fix_pos = -1;
            g_fix_count = 0;
            g_resync_fails = 0;
            if (g_mv_restore_pending) {
                /* Publish the restore seek before decoder start; do not ack it
                 * in the generation reset or initialise local_seek past it. */
                g_seek_req = g_mv_saved.pos;
                g_seek_seq++;
                g_mv_initial_seek = 1;
                g_mv_reopening = 1;
                g_mv_restore_pending = 0;
                g_dec_ack = g_seek_seq - 1;
            } else {
                g_mv_initial_seek = 0;
                g_mv_reopening = 0;
                g_dec_ack = g_seek_seq;
            }
            local_ack = g_dec_ack;
            g_starve = 0;
            g_starve_max_ms = 0;
            reset_resampler();
            g_src_rate = OUT_RATE;
            g_channels = 2;
            g_kbps = 0;

            g_dec_thid = sceKernelCreateThread("melody_dec", decode_thread, PRIO_DEC,
                                               STACK_SIZE, 0, NULL);
            if (g_dec_thid < 0) {
                mlog("decode thread create fail");
                g_dec_thid = -1;
                g_state = AUDIO_ERROR;
                snprintf(g_errmsg, sizeof(g_errmsg), "线程创建失败");
                sceKernelDelayThread(100 * 1000);
                continue;
            }
            sceKernelStartThread(g_dec_thid, 0, NULL);

            if (g_channel < 0) {
                g_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OUT_SAMPLES,
                                              PSP_AUDIO_FORMAT_STEREO);
                if (g_channel < 0) {
                    mlog("ChReserve = 0x%08X", g_channel);
                    g_state = AUDIO_ERROR;
                    snprintf(g_errmsg, sizeof(g_errmsg), "音频输出失败");
                    g_channel = -1;
                    continue;
                }
            }
        }

        /* seek 完成（decode 线程回 ack）：把显示位置对到它实际落的地方 */
        if (g_dec_ack != local_ack) {
            local_ack = g_dec_ack;
            g_seek_ms = g_seek_applied;
            g_pos_samples = 0;
            reset_resampler();
        }

        if (g_pause) {
            if (g_state != AUDIO_PAUSE) {
                g_state = AUDIO_PAUSE;
                set_state(AUDIO_PAUSE, "pause");
            }
            if (!audio_skin_silence()) sceKernelDelayThread(20 * 1000);
            continue;
        }

        avail = (int)(g_ring_w - g_ring_r);

        if (avail < OUT_SAMPLES) {
            /* 环空了：出静音撑住，不重放上一块 */
            unsigned now = sceKernelGetSystemTimeLow();

            memset(g_outbuf, 0, sizeof(g_outbuf));
            if (g_dec_done && g_state!=AUDIO_ERROR) {
                if (g_state != AUDIO_END) set_state(AUDIO_END, "drained");
                g_state = AUDIO_END;
                if (!audio_skin_silence()) sceKernelDelayThread(20 * 1000);
                continue;
            }
            if (g_state != AUDIO_ERROR) g_state = AUDIO_BUFFER;
            g_starve++;
            if (g_last_pop_us) {
                unsigned gap = (now - g_last_pop_us) / 1000;

                if ((int)gap < 30000 && (int)gap > g_starve_max_ms) g_starve_max_ms = (int)gap;
            }
            skin_sound_mix(g_outbuf, OUT_SAMPLES, 2, OUT_RATE);
            audio_skin_output();
            g_last_pop_us = sceKernelGetSystemTimeLow();
            continue;
        }

        ring_pop(g_outbuf, OUT_SAMPLES);

        /* 音量放在输出这一侧：调音量立刻生效，不用等环里 2 秒的存货放完 */
        if (g_vol < 100) {
            int vol = g_vol;

            for (int i = 0; i < OUT_SAMPLES * 2; ++i) {
                int v2 = ((int)g_outbuf[i] * vol + 50) / 100;

                if (v2 > 32767) v2 = 32767;
                else if (v2 < -32768) v2 = -32768;
                g_outbuf[i] = (short)v2;
            }
        }

        int measured=audio_measure_level(g_outbuf,OUT_SAMPLES,2);int prior=g_music_level;g_music_level=measured>prior?(prior+measured)/2:(prior*7+measured)/8;
        skin_sound_mix(g_outbuf, OUT_SAMPLES, 2, OUT_RATE);
        audio_skin_output();
        g_last_pop_us = sceKernelGetSystemTimeLow();
        g_pos_samples += OUT_SAMPLES;
        /* 别把 decode 线程报的 ERROR/END 覆盖掉 */
        if (g_state == AUDIO_PLAY || g_state == AUDIO_BUFFER)
            g_state = (g_ring_w - g_ring_r < (unsigned)(OUT_RATE / 2)) ? AUDIO_BUFFER : AUDIO_PLAY;

        if (g_ring_w - g_ring_r >= (unsigned)(OUT_RATE / 2) && g_last_state != AUDIO_PLAY)
            set_state(AUDIO_PLAY, "ok");

        {
            unsigned now = (unsigned)(sceKernelGetSystemTimeLow() / 1000);

            if (now - hb_ms >= 4000) {
                hb_ms = now;
                mlog("hb pos=%dms state=%d ring=%d%% starve=%u maxgap=%dms fed=%lld pend=%d",
                     audio_pos_ms(), (int)g_state,
                     (int)((g_ring_w - g_ring_r) * 100 / RING_FRAMES), g_starve,
                     g_starve_max_ms, (long long)g_read_pos, g_pending_hint);
                g_starve = 0;
                g_starve_max_ms = 0;
            }
        }
    }

    if (retire_decode_thread() < 0) return -1;
    close_mp3();
    if (g_file >= 0) {
        sceIoClose(g_file);
        g_file = -1;
    }
    if (g_channel >= 0) {
        sceAudioChRelease(g_channel);
        g_channel = -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int audio_file_ok(const char *path)
{
    SceUID fd;
    long long size;
    static unsigned char buf[4096];
    int tried = 0, good = 0;

    fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;
    size = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (size < 128 * 1024) {
        sceIoClose(fd);
        return 0;
    }

    for (int k = 1; k <= 8; ++k) {
        long long off = size * k / 9;
        int got, found = 0, i;

        if (sceIoLseek(fd, (SceOff)off, PSP_SEEK_SET) < 0) continue;
        got = sceIoRead(fd, buf, sizeof(buf));
        if (got < 64) continue;
        tried++;

        for (i = 0; i + 16 < got && !found; ++i) {
            int j = i, n = 0;

            while (n < 4) {
                int fl = mp3_frame_len(buf + j, NULL, NULL, NULL);

                if (fl <= 0 || j + fl + 4 > got) break;
                j += fl;
                n++;
            }
            if (n >= 4) found = 1;
        }
        if (found) good++;
    }
    sceIoClose(fd);

    if (tried == 0) return 0;
    return good * 100 / tried >= 85;
}

int audio_probe_duration(const char *path, int *kbps_out)
{
    unsigned char hdr[4096];
    SceUID fd;
    int got, i, start = 0, br = 0, sr = 0;
    long long size;

    fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;
    size = (long long)sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    got = sceIoRead(fd, hdr, sizeof(hdr));
    sceIoClose(fd);
    if (got < 16) return 0;

    if (!memcmp(hdr, "ID3", 3)) {
        start = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
        start += 10;
        if (start >= got) start = 0;
    }

    for (i = start; i + 4 <= got; ++i) {
        if (mp3_frame_len(hdr + i, &sr, &br, NULL) > 0) break;
    }
    if (i + 4 > got || br <= 0) return 0;

    if (kbps_out) *kbps_out = br;
    return (int)((size - start) * 8 / br);
}

int audio_init(void)
{
    int st;

    st = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    mlog("LoadModule AVCODEC = 0x%08X", st);
    st = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    mlog("LoadModule MP3 = 0x%08X", st);

    st = sceMp3InitResource();
    mlog("sceMp3InitResource = 0x%08X", st);
    if (st < 0) return -2;

    g_state = AUDIO_IDLE;
    g_gen = 1;

    g_out_thid = sceKernelCreateThread("melody_audio", audio_thread, PRIO_OUT,
                                       STACK_SIZE, 0, NULL);
    if (g_out_thid < 0) return -3;

    sceKernelStartThread(g_out_thid, 0, NULL);
    mlog("audio ready (decode thread + %dKB ring)", RING_FRAMES * 4 / 1024);
    return 0;
}

void audio_shutdown(void)
{
    SceUInt timeout = 500 * 1000;
    int r;

    g_quit = 1;
    g_state = AUDIO_IDLE;

    if (g_out_thid >= 0) {
        r = sceKernelWaitThreadEnd(g_out_thid, &timeout);
        if (r < 0) {
            g_audio_faulted = 1;
            mlog("audio: output shutdown incomplete, resources retained 0x%08X", r);
            return;
        }
        sceKernelDeleteThread(g_out_thid);
        g_out_thid = -1;
    }
    if (retire_decode_thread() < 0) return;

    if (g_channel >= 0) {
        sceAudioChRelease(g_channel);
        g_channel = -1;
    }
    close_mp3();
    sceMp3TermResource();
}

int audio_play_stream(const Song *s, const char *url, int total)
{
    int my;

    if (!s || g_audio_faulted || g_mv_held || g_mv_request) return -1;

    g_state = AUDIO_IDLE;

    cache_path(g_path, sizeof(g_path), 0, s->id);
    g_local = 0;
    g_file_bytes = 0;
    g_total_bytes = total;
    g_dur_ms = s->dur_ms;
    g_seek_ms = 0;
    g_seek_req = 0;
    g_errmsg[0] = 0;
    g_cached_pct = 0;
    g_pause = 0;

    g_gen++;
    my = g_gen;

    if (dl_stream_start(s, url, total, my) < 0) return -1;
    if (g_audio_faulted) { dl_stream_stop(); return -1; }

    g_state = AUDIO_BUFFER;
    if (g_audio_faulted) return -1;
    mlog("audio stream %lld %s (%dKB)", s->id, s->name, total / 1024);
    return my;
}

int audio_play_file(const char *path, const Song *meta)
{
    int my;

    if (!path || !path[0] || g_audio_faulted || g_mv_held || g_mv_request) return -1;

    g_state = AUDIO_IDLE;

    snprintf(g_path, sizeof(g_path), "%s", path);
    g_local = 1;

    {
        int fd = sceIoOpen(g_path, PSP_O_RDONLY, 0777);

        if (fd >= 0) {
            static unsigned char probe[512];
            int n1 = sceIoRead(fd, probe, sizeof(probe));
            int nz = 0;

            for (int i = 0; i < n1; ++i) if (probe[i]) nz++;
            if (n1 > 0 && nz == 0) mlog("file head is all zero (%d bytes)", n1);

            if (sceIoLseek(fd, -512, PSP_SEEK_END) >= 0) {
                int n2 = sceIoRead(fd, probe, sizeof(probe));

                nz = 0;
                for (int i = 0; i < n2; ++i) if (probe[i]) nz++;
                if (n2 > 0 && nz == 0) mlog("file tail is all zero (%d bytes)", n2);
            }
            sceIoClose(fd);
        } else {
            mlog("open failed %s (%d)", g_path, fd);
            snprintf(g_errmsg, sizeof(g_errmsg), "文件打不开");
            g_state = AUDIO_ERROR;
            return -1;
        }
    }

    {
        SceIoStat st;

        if (sceIoGetstat(g_path, &st) < 0 || st.st_size <= 0) {
            snprintf(g_errmsg, sizeof(g_errmsg), "文件是空的");
            g_state = AUDIO_ERROR;
            return -1;
        }
        g_file_bytes = (long long)st.st_size;
    }

    g_total_bytes = g_file_bytes;
    g_dur_ms = meta ? meta->dur_ms : 0;
    g_seek_ms = 0;
    g_seek_req = 0;
    g_errmsg[0] = 0;
    g_cached_pct = -1;
    g_pause = 0;

    g_gen++;
    my = g_gen;

    if (g_audio_faulted) return -1;
    g_state = AUDIO_BUFFER;
    if (g_audio_faulted) return -1;
    mlog("audio file %s (%dKB)", g_path, (int)(g_file_bytes / 1024));
    return my;
}

int audio_play_cached(const Song *s)
{
    char path[128];

    if (!s) return -1;

    if (cache_file_bytes(s->id) <= 0) {
        snprintf(g_errmsg, sizeof(g_errmsg), "缓存文件不见了");
        g_state = AUDIO_ERROR;
        return -1;
    }
    cache_path(path, sizeof(path), 0, s->id);
    return audio_play_file(path, s);
}

void audio_stop(void)
{
    g_state = AUDIO_IDLE;
    g_pause = 0;
    dl_stream_stop();
}


int audio_replay_current(const Song *meta)
{
    char path[sizeof(g_path)];
    Song song;
    if(g_audio_faulted||g_mv_held||g_mv_request||!g_path[0])return -1;
    if(!g_local&&!dl_stream_done(g_gen))return -1;
    /* audio_play_file writes g_path; never pass that same storage as its source. */
    snprintf(path,sizeof(path),"%s",g_path);
    if(meta)song=*meta;
    else {memset(&song,0,sizeof(song));song.dur_ms=g_dur_ms;}
    audio_stop();
    return audio_play_file(path,&song);
}

void audio_pause(int on)
{
    g_pause = on ? 1 : 0;
    if (!on && g_state == AUDIO_PAUSE) g_state = AUDIO_PLAY;
}

int audio_paused(void) { return g_pause; }
int audio_state(void) { return g_audio_faulted ? AUDIO_ERROR : g_state; }
int audio_needs_restart(void) { return g_audio_faulted != 0; }
int audio_dur_ms(void) { return g_dur_ms; }
int audio_rate(void) { return g_rate; }
int audio_kbps(void) { return g_kbps; }
const char *audio_error(void) { return g_audio_faulted ? "音频未能停止，请重启" : g_errmsg; }

void audio_seek(int ms)
{
    int upper;
    if (g_dur_ms <= 0) return;
    upper = g_dur_ms > 800 ? g_dur_ms - 800 : 0;
    if (ms < 0) ms = 0;
    if (ms > upper) ms = upper;

    g_seek_req = ms;
    g_seek_seq++;
    mlog("seek req %dms (seq %d)", ms, (int)g_seek_seq);
}

int audio_pos_ms(void)
{
    long long ms = (long long)g_pos_samples * 1000 / (g_rate > 0 ? g_rate : 44100);

    return (int)ms + g_seek_ms;
}

int audio_cache_pct(void)
{
    int total;

    if (g_local) return -1;

    total = g_total_bytes;
    if (total <= 0) return 0;
    if (dl_stream_done(g_gen)) return -1;

    {
        long long have = dl_stream_bytes();
        int pct = (int)(have * 100 / total);

        if (pct > 99) pct = 99;
        g_cached_pct = pct;
        return pct;
    }
}

int audio_buffered_ms(void)
{
    long long avail;

    if (g_local) return 0;
    avail = dl_stream_available(g_gen);
    if (g_total_bytes <= 0 || g_dur_ms <= 0) return 0;
    return (int)(avail * g_dur_ms / g_total_bytes);
}

int audio_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_vol = v;
    return g_vol;
}

int audio_volume(void) { return g_vol; }
