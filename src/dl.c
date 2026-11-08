#include "thread_paths.h"
#include "dl.h"
#include "dl_progress_contract.h"
#include "audio.h"

#include "cache.h"
#include "log.h"
#include "net.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DL_MAX_PARTS   6
#define DL_MAX_CHUNKS  320
#define DL_CHUNK_SIZE  (96 * 1024)
#ifndef DL_QUEUE_MAX
#define DL_QUEUE_MAX 32
#endif
#define DL_MAX_QUEUE DL_QUEUE_MAX
#define DL_STACK       0x8000
 #define DL_PRIO        0x1a          /* 26：低于音频(20/21)和 UI(24)；比 api(30) 高 */
#define DL_MAX_RETRY   4
#define DL_RUN_CHUNKS  8      /* 开播前：一段约 768KB，前缀连续 */
#define DL_RUN_SMALL   4      /* 之后：一段至少 384KB */
 #define DL_PREBUF      (320 * 1024)   /* 前缀到 320KB 之前只用一条连接 */
#define DL_WBUF        32768  /* 每个连接的写缓冲：一次写一个记忆棒簇，省 FAT 事务 */

typedef struct {
    volatile int active;
    volatile int complete;
    volatile int error;
     volatile int total;              /* 0 = 不知道长度 */
    volatile int nchunks;
    volatile int done_chunks;
    volatile long long done_bytes;
    volatile int gen;
    int fd;

    int nparts;
    int chunk_size;

    unsigned char cstate[DL_MAX_CHUNKS];   /* 0 待取 1 已分配 2 完成 */
    volatile int cgot[DL_MAX_CHUNKS];
    unsigned char cretry[DL_MAX_CHUNKS];
    int cpart[DL_MAX_CHUNKS];

    /* 写缓冲（每连接）：woff 是缓冲里第一字节的文件偏移 */
    int wlen[DL_MAX_PARTS];
    long long woff[DL_MAX_PARTS];

    Http http[DL_MAX_PARTS];
     long long pbytes[DL_MAX_PARTS];     /* 统计：每条连接本轮下了多少 */
    long long ppos[DL_MAX_PARTS];       /* 下一条要写的绝对偏移 */
     long long pend[DL_MAX_PARTS];       /* 这一段范围的结束（不含） */
     int prun[DL_MAX_PARTS];             /* 这一段范围的起始分片 */
     int prun_end[DL_MAX_PARTS];         /* 结束分片（不含） */

    long long id;
    Song song;
    char url[1000];
    char path[128];
    char err[96];
    int is_stream;
    int keep_queued;

    unsigned int t0;
    unsigned int t_log;

    /* 卡死检测 */
    long long watch_bytes;      /* 上次看门狗记的进度 */
    unsigned int watch_t;       /* 上次有进展的时间 */
    int stall_n;                /* 连续重连次数 */
    volatile int read_off;      /* audio reader demand, only accepted for this gen */
    long long watch_prefix;
    unsigned int prefix_t;
    long long saved_prefix;
    unsigned int prefix_hash;
    unsigned int checkpoint_t;
    int checkpoint_dirty;
    unsigned int next_open_at;
    unsigned int retry_at[DL_MAX_PARTS];
    unsigned int retry_ms[DL_MAX_PARTS];
} DlJob;

static DlJob g_stream;
static DlJob g_qjob;
static DlItem g_queue[DL_MAX_QUEUE];
static char g_urls[DL_MAX_QUEUE][1000];
static int g_queue_n;
static volatile int g_quit;
static volatile int g_io_epoch;
static volatile int g_open_background;
static volatile int g_video_priority;
void dl_set_video_priority(int enabled){g_video_priority=enabled!=0;}
static int g_thid = -1;
static volatile int g_qgen;
static volatile int g_speed;
static int g_parts = 4;

/* 每个连接一个写缓冲：16KB 的 recv 会落在记忆棒簇（通常 32KB）的中间，
 * 直接写会触发 FAT 的读-改-写。攒满一个簇再写，写次数和擦除都减半。
 * 同一时刻只有一个 job 在跑（播放流优先），所以缓冲区可以共用。 */
static unsigned char g_wbuf[DL_MAX_PARTS][DL_WBUF];

 /* 播放流的「开始/停止」命令：只允许 dl 线程去动 g_stream，
 * 别的线程投命令，避免两个线程同时 reset 同一个 job */
static int g_lock = -1;
static volatile int g_cmd_pending;
static volatile int g_cmd_stop;
static volatile int g_cmd_qcancel;
static volatile int g_cmd_qclear;
static long long g_cmd_qpause_id;
static long long g_cmd_qcancel_id;
static int g_queue_dirty;
static Song g_cmd_song;
static char g_cmd_url[1000];
static int g_cmd_total;
static int g_cmd_gen;
static int g_read_gen;
static int g_read_off;

static void job_finish(DlJob *j);
static void checkpoint_save(DlJob *j);
static int checkpoint_load(DlJob *j);
static long long job_prefix(DlJob *j);
static long long advance_chunks(DlJob *j, long long from, int n);

static void dl_lock(void) { if (g_lock >= 0) sceKernelWaitSema(g_lock, 1, NULL); }
static void dl_unlock(void) { if (g_lock >= 0) sceKernelSignalSema(g_lock, 1); }

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static int chunk_len(DlJob *j, int i)
{
    long long off, left;

    if (j->total <= 0) return 0;
    off = (long long)i * j->chunk_size;
    left = (long long)j->total - off;
    if (left <= 0) return 0;
    return (int)(left < (long long)j->chunk_size ? left : (long long)j->chunk_size);
}

static long long chunk_off(DlJob *j, int i)
{
    return (long long)i * j->chunk_size;
}

static void job_reset(DlJob *j)
{
    memset(j, 0, sizeof(*j));
    for (int p = 0; p < DL_MAX_PARTS; ++p) {
        j->http[p].sock = -1;
        j->prun[p] = -1;
        j->prun_end[p] = -1;
    }
    j->fd = -1;
}

static void job_close_net(DlJob *j)
{
    for (int p = 0; p < DL_MAX_PARTS; ++p) {
        if (j->http[p].sock >= 0) http_close(&j->http[p]);
        j->prun[p] = -1;
        j->prun_end[p] = -1;
        j->ppos[p] = 0;
        j->pend[p] = 0;
        j->wlen[p] = 0;
    }
    for (int i = 0; i < j->nchunks; ++i) {
         if (j->cstate[i] == 1) j->cstate[i] = 0;   /* cgot 保留，用来续传 */
        j->cpart[i] = -1;
    }
}

static void job_close_file(DlJob *j)
{
    if (j->fd >= 0) {
        checkpoint_save(j);
        sceIoClose(j->fd);
        j->fd = -1;
    }
}

/* 「未完成」标记：<id>.part。有它说明文件可能是半截的，启动时不要当缓存认领。
 * 下载真正完成（cache_add 成功）才删掉。 */
static int part_marker(DlJob *j, int create)
{
    char path[128];

    snprintf(path, sizeof(path), APP_DIR "cache/%lld.part", j->id);
    if (create) {
        int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd < 0) return -1;
        return sceIoClose(fd);
    }
    return sceIoRemove(path);
}

/* 把某个连接攒的写缓冲刷进文件；写满整个长度，短写不会让文件错位 */
static int flush_part(DlJob *j, int p)
{
    int total = j->wlen[p];
    int done = 0;

    if (total <= 0) return 0;
    if (j->fd < 0) { j->wlen[p] = 0; return -1; }

    while (done < total) {
        int w;

        if (sceIoLseek(j->fd, (SceOff)(j->woff[p] + done), PSP_SEEK_SET) < 0) {
            j->error = 1;
            snprintf(j->err, sizeof(j->err), "seek failed");
            return -1;
        }
        w = sceIoWrite(j->fd, g_wbuf[p] + done, total - done);
        if (w <= 0) {
            j->error = 1;
            snprintf(j->err, sizeof(j->err), "write failed (%d)", w);
            return -1;
        }
        done += w;
    }

    /* Only bytes that closed a hole count as progress. Refetching an already
     * counted range (reconnect, re-assigned run) must not move the bar. */
    j->done_bytes = dl_clamp_done(j->done_bytes + advance_chunks(j, j->woff[p], total),
                                  j->total > 0 ? j->total : 0);
    j->woff[p] += total;
    j->pbytes[p] += total;
    j->wlen[p] = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
 /* 任务开始 / 结束                                                     */
/* ------------------------------------------------------------------ */

static int job_begin(DlJob *j, const Song *song, const char *url, int total,
                     int is_stream, int gen)
{
    int parts, resumed;

    job_reset(j);
    j->id = song ? song->id : 0;
    if (song) j->song = *song;
    snprintf(j->url, sizeof(j->url), "%s", url);
    j->total = total;
    j->gen = gen;
    j->is_stream = is_stream;

    if (total > 0) {
        j->chunk_size = DL_CHUNK_SIZE;
        j->nchunks = (total + DL_CHUNK_SIZE - 1) / DL_CHUNK_SIZE;
        if (j->nchunks > DL_MAX_CHUNKS) {
            j->chunk_size = (total + DL_MAX_CHUNKS - 1) / DL_MAX_CHUNKS;
            j->chunk_size = (j->chunk_size + 16383) & ~16383;
            j->nchunks = (total + j->chunk_size - 1) / j->chunk_size;
        }
    } else {
        j->chunk_size = 0;
        j->nchunks = 1;
    }

    parts = g_parts;
    if (parts < 1) parts = 1;
    if (parts > DL_MAX_PARTS) parts = DL_MAX_PARTS;
    if (total > 0 && j->nchunks < parts) parts = j->nchunks;
    if (total <= 0) parts = 1;
    j->nparts = parts;

    cache_path(j->path, sizeof(j->path), 0, j->id);
    resumed = checkpoint_load(j);
    j->fd = sceIoOpen(j->path, PSP_O_RDWR | PSP_O_CREAT | (resumed ? 0 : PSP_O_TRUNC), 0777);

    /* 音频线程可能正开着这个文件在读（播放中切歌），等它松手再试 */
    for (int i = 0; i < 5 && j->fd < 0; ++i) {
        sceKernelDelayThread(120 * 1000);
        j->fd = sceIoOpen(j->path, PSP_O_RDWR | PSP_O_CREAT | (resumed ? 0 : PSP_O_TRUNC), 0777);
    }

    if (j->fd < 0) {
        snprintf(j->err, sizeof(j->err), "cannot write cache file");
        j->error = 1;
        mlog("dl: open %s failed (0x%08X)", j->path, j->fd);
        return -1;
    }

    /* Preserve a validated checkpoint while extending its immutable prefix.
     * Old empty .part markers safely restart at zero.
     * The marker is one more FAT create right after the cache file was opened;
     * a transient failure here used to kill the whole stream silently (no
     * "dl start stream" line, decode then reported 没有数据). Retry like open. */
    if (!resumed) {
        int marker_ok = part_marker(j, 1) == 0;

        for (int i = 0; i < 4 && !marker_ok; ++i) {
            sceKernelDelayThread(120 * 1000);
            marker_ok = part_marker(j, 1) == 0;
        }
        if (!marker_ok) {
            snprintf(j->err, sizeof(j->err), "cannot create incomplete marker");
            j->error = 1;
            mlog("dl: marker %s.part failed", j->path);
            job_close_file(j);
            return -1;
        }
    }
    if (!resumed) j->prefix_hash = 2166136261u;

    for (int p = 0; p < DL_MAX_PARTS; ++p) {
        j->wlen[p] = 0;
        j->woff[p] = 0;
    }

    j->t0 = sceKernelGetSystemTimeLow();
    j->t_log = j->t0;
    j->active = 1;
    mlog("dl start %s id=%lld total=%d chunks=%d parts=%d", is_stream ? "stream" : "queue",
         j->id, total, j->nchunks, j->nparts);
    return 0;
}

static void job_finish(DlJob *j)
{
    long long bytes = j->done_bytes;

    /* 先把还在写缓冲里的数据落盘，再关文件 */
    for (int p = 0; p < DL_MAX_PARTS; ++p) {
        if (j->wlen[p] > 0) flush_part(j, p);
    }
    bytes = j->done_bytes;

    job_close_net(j);
    job_close_file(j);
    j->active = 0;

    if (j->error) {
        mlog("dl failed id=%lld: %s (%lldKB)", j->id, j->err, bytes / 1024);
        g_qgen++; g_queue_dirty = 1;
        return;     /* 保留 <id>.part：下次启动不会被当成完整缓存认领 */
    }

    if (j->total <= 0) j->total = (int)bytes;

    if (bytes > 8192) {
        /* 只记日志，不删文件：删文件会让正在播放的音频线程读到 EOF
         * （表现就是随机重播 / 卡顿），要清也得等它不播了再清 */
        if (!audio_file_ok(j->path))
            mlog("dl warn: id=%lld maybe incomplete (%lldKB)", j->id, bytes / 1024);
        cache_add(&j->song, (int)bytes);
        part_marker(j, 0);
        {
            int ms = (int)((sceKernelGetSystemTimeLow() - j->t0) / 1000);
            mlog("dl done id=%lld %dKB in %dms = %dKB/s", j->id, (int)bytes / 1024, ms,
                 ms > 0 ? (int)(bytes / 1024 * 1000 / ms) : 0);
        }
    } else {
        snprintf(j->err, sizeof(j->err), "empty download");
        j->error = 1;
        mlog("dl empty id=%lld (%lld bytes)", j->id, bytes);
    }

    j->complete = !j->error;
    if (j->complete) part_marker(j, 0);
    g_qgen++; g_queue_dirty = 1;
}

/* ------------------------------------------------------------------ */
 /* 分片 + 连续段调度                                                   */
/* ------------------------------------------------------------------ */

 /* 从 0 开始已经连续下载好的字节数 */
static long long job_prefix(DlJob *j)
{
    long long p = 0;

    if (j->total <= 0) return j->cgot[0];

    for (int i = 0; i < j->nchunks; ++i) {
        if (j->cstate[i] == 2) { p += chunk_len(j, i); continue; }
        /* cgot counts disk writes, including a disconnected/unassigned chunk. */
        p += j->cgot[i];
        break;
    }
    return p;
}

/* .part v1 deliberately records only the verified contiguous prefix. Sparse
 * extents are overwritten after restart. Metadata binds id, length and exact
 * URL; FNV-1a detects torn metadata/data, not hostile content authenticity.
 * Re-signed/different URLs restart conservatively even when the id is equal. */
static unsigned int hash_bytes(unsigned int h, const unsigned char *buf, int n)
{
    for (int i = 0; i < n; ++i) h = (h ^ buf[i]) * 16777619u;
    return h;
}

static int hash_disk(int fd, long long from, long long to, unsigned int *hash)
{
    unsigned char buf[4096];
    if (sceIoLseek(fd, from, PSP_SEEK_SET) != from) return -1;
    while (from < to) {
        int want = to - from > (long long)sizeof(buf) ? (int)sizeof(buf) : (int)(to - from);
        int n = sceIoRead(fd, buf, want);
        if (n <= 0) return -1;
        *hash = hash_bytes(*hash, buf, n);
        from += n;
    }
    return 0;
}

static int checkpoint_load(DlJob *j)
{
    char path[128], text[160], tail;
    long long id, prefix, size;
    int fd, n, total;
    unsigned int hash, url_hash, actual = 2166136261u;
    if (j->total <= 0) return 0; /* unknown-length EOF cannot prove a restart */
    snprintf(path, sizeof(path), APP_DIR "cache/%lld.part", j->id);
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (n <= 0 || n >= (int)sizeof(text) - 1) return 0;
    text[n] = 0;
    if (sscanf(text, "MDL1 %lld %d %lld %u %u %c", &id, &total, &prefix,
               &hash, &url_hash, &tail) != 5 || id != j->id || total != j->total ||
        prefix <= 0 || prefix > total ||
        url_hash != hash_bytes(2166136261u, (const unsigned char *)j->url, (int)strlen(j->url)))
        return 0;
    fd = sceIoOpen(j->path, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    size = sceIoLseek(fd, 0, PSP_SEEK_END);
    n = size >= prefix && size <= j->total ? hash_disk(fd, 0, prefix, &actual) : -1;
    sceIoClose(fd);
    if (n < 0 || actual != hash) return 0;
    for (int i = 0; i < j->nchunks && chunk_off(j, i) < prefix; ++i) {
        long long remain = prefix - chunk_off(j, i);
        int len = chunk_len(j, i);
        j->cgot[i] = remain < len ? (int)remain : len;
        if (j->cgot[i] == len) { j->cstate[i] = 2; j->done_chunks++; }
    }
    j->done_bytes = prefix;
    j->saved_prefix = prefix;
    j->prefix_hash = hash;
    return 1;
}

static void checkpoint_save(DlJob *j)
{
    char path[128], text[160];
    long long prefix;
    unsigned int hash;
    int fd, n, done = 0;
    if (j->fd < 0 || j->complete || j->total <= 0) return;
    prefix = job_prefix(j);
    if (prefix <= 0 || prefix < j->saved_prefix || prefix > j->total ||
        (prefix == j->saved_prefix && !j->checkpoint_dirty)) return;
    hash = j->saved_prefix ? j->prefix_hash : 2166136261u;
    if (hash_disk(j->fd, j->saved_prefix, prefix, &hash) < 0) return;
    /* Flush data before publishing the checkpoint. A torn marker is rejected
     * on load; no file-length shortcut ever promotes a sparse extent. */
    if (sceIoSync("ms0:", 0) < 0) return;
    snprintf(path, sizeof(path), APP_DIR "cache/%lld.part", j->id);
    n = snprintf(text, sizeof(text), "MDL1 %lld %d %lld %u %u\n", j->id, j->total, prefix,
                 hash, hash_bytes(2166136261u, (const unsigned char *)j->url, (int)strlen(j->url)));
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    while (done < n) {
        int w = sceIoWrite(fd, text + done, n - done);
        if (w <= 0) break;
        done += w;
    }
    sceIoClose(fd);
    if (done == n) {
        j->saved_prefix = prefix;
        j->prefix_hash = hash;
        j->checkpoint_t = sceKernelGetSystemTimeLow();
        j->checkpoint_dirty = 0;
    }
}

/* The low watermark follows real reader demand, rather than total download %. */
static int prefix_low(DlJob *j)
{
    return j->is_stream && job_prefix(j) - j->read_off < DL_PREBUF;
}

static int prefix_part(DlJob *j)
{
    long long prefix = job_prefix(j);
    for (int p = 0; p < j->nparts; ++p) {
        if (j->prun_end[p] > j->prun[p] &&
            j->ppos[p] - j->wlen[p] == prefix && j->ppos[p] < j->pend[p])
            return p;
    }
    return -1;
}

/* 这个分片是不是正被某条活着的连接下着 */
static int chunk_owned(DlJob *j, int ci)
{
    for (int p = 0; p < j->nparts; ++p) {
        if (j->http[p].sock < 0) continue;
        if (j->prun_end[p] > j->prun[p] && ci >= j->prun[p] && ci < j->prun_end[p]) {
            if (j->ppos[p] < j->pend[p]) return 1;
        }
    }
    return 0;
}

static int next_pending(DlJob *j)
{
    for (int i = 0; i < j->nchunks; ++i) {
        if (j->cstate[i] == 2) continue;
        /* cstate==1 但没人下 = 孤儿分片（连接提前断了），要接回来续传，
         * 否则 done_chunks 永远差几个 -> 卡在 99% */
        if (j->cstate[i] == 1 && chunk_owned(j, i)) continue;
        return i;
    }
    return -1;
}

 /* 把 [from, from+n) 记到分片状态里（连续写，跨分片也能算对）。
  * 返回这一段里「新补上的字节数」：已完成分片只算缺的那一截，
  * 重复写同一范围返回 0，避免 done_bytes 超过 total（进度>100%）。 */
static long long advance_chunks(DlJob *j, long long from, int n)
{
    int ci;
    long long to = from + n;
    long long added = 0;

    if (j->total <= 0 || j->chunk_size <= 0) {
        j->cgot[0] += n;
        j->cstate[0] = 1;
        return n;
    }
    if (from < 0) from = 0;
    if (to > j->total) to = j->total;
    if (to <= from) return 0;

    ci = (int)(from / j->chunk_size);
    while (ci < j->nchunks && chunk_off(j, ci) < to) {
        long long cstart = chunk_off(j, ci);
        int clen = chunk_len(j, ci);

        if (to >= cstart + clen) {
            if (j->cstate[ci] != 2) {
                added += dl_count_new_bytes(cstart + j->cgot[ci], cstart + clen, j->total);
                j->cgot[ci] = clen;
                j->cstate[ci] = 2;
                j->cpart[ci] = -1;
                j->done_chunks++;
            }
        } else {
            long long have = cstart + j->cgot[ci];
            added += dl_count_new_bytes(have, to, j->total);
            if (to - cstart > j->cgot[ci]) j->cgot[ci] = (int)(to - cstart);
            /* Never un-complete a chunk by rewriting one of its sub-ranges. */
            if (j->cstate[ci] != 2) j->cstate[ci] = 1;
        }
        ci++;
    }
    return added;
}

static void part_drop(DlJob *j, int p)
{
    if (j->http[p].sock >= 0) http_close(&j->http[p]);
}

/* 这一段要拿几个分片：开播前小段（前缀快），之后按「每条连接两轮」切，
 * 既把握手/slow-start 从十几次降下来，又留一轮给跑得快的连接去抢，
 * 不会因为某条连接慢就把整首歌拖住。 */
static int run_chunks(DlJob *j)
{
    int big;

    /* One chunk per stream request: a fast idle connection can take the next
     * earliest hole instead of reserving a distant multi-chunk run. */
    if (j->is_stream) return 1;

    big = (j->nchunks + j->nparts * 2 - 1) / (j->nparts * 2);
    if (big < DL_RUN_SMALL) big = DL_RUN_SMALL;
    if (big > j->nchunks) big = j->nchunks;
    return big;
}

/* Retry deadlines use unsigned microseconds and signed wrap-safe comparisons;
 * no sleeping in a handshake failure path can starve the other job. */
static void retry_later(DlJob *j, int p)
{
    unsigned int ms = j->retry_ms[p] ? j->retry_ms[p] * 2 : 250;
    if (ms > 8000) ms = 8000;
    j->retry_ms[p] = ms;
    j->retry_at[p] = sceKernelGetSystemTimeLow() + ms * 1000u;
}

static int open_part(DlJob *j, int p, long long off, long long to, char *err, int len)
{
    int epoch = g_io_epoch, r;
    if (g_quit || g_cmd_pending || g_cmd_stop ||
        (!j->is_stream && (g_cmd_qcancel || g_cmd_qclear))) return -3;
    g_open_background = !j->is_stream;
    /* Reader demand may have arrived after the foreground pump, before this
     * background open. Arm its cancellation flag before checking that demand. */
    if (!j->is_stream && g_stream.active && !g_stream.complete &&
        g_read_gen == g_stream.gen && job_prefix(&g_stream) - g_read_off < DL_PREBUF) {
        g_open_background = 0;
        return -3;
    }
    net_reconnect_poll();
    r = http_open_range_cancel(&j->http[p], j->url, off, to, 8000,
                               &g_io_epoch, epoch, err, len);
    g_open_background = 0;
    if (g_io_epoch != epoch || g_quit) return -3;
    if (r < 0) { retry_later(j, p); j->next_open_at = j->retry_at[p]; }
    else { j->retry_ms[p] = 0; j->retry_at[p] = 0; }
    return r;
}

 /* 给空闲连接分配一段连续分片；有没跑完的范围就先续传 */
static void assign_parts(DlJob *j)
{
    if (j->next_open_at && (int)(sceKernelGetSystemTimeLow() - j->next_open_at) < 0) return;
    for (int p = 0; p < j->nparts; ++p) {
        int ci, endc, i;
        long long off, endoff;
        char err[96] = "";

        if (j->http[p].sock >= 0) continue;
        if (j->retry_ms[p] && (int)(sceKernelGetSystemTimeLow() - j->retry_at[p]) < 0)
            continue;

        /* At low water only the connection filling the first hole may open.
         * It need not be part zero after an out-of-order completion. */
        if (prefix_low(j)) {
            int owner = prefix_part(j);
            if (owner >= 0 && owner != p) continue;
            if (owner < 0 && j->chunk_size > 0 &&
                next_pending(j) != (int)(job_prefix(j) / j->chunk_size))
                continue;
            if (owner < 0) {
                if (flush_part(j, p) < 0) return;
                j->prun[p] = -1;
                j->prun_end[p] = -1;
            }
        }

        if (j->prun_end[p] > j->prun[p] && j->ppos[p] < j->pend[p]) {
             /* ppos includes received bytes; publish any pending write before
              * resuming there, otherwise reconnect silently skips a disk hole. */
            if (flush_part(j, p) < 0) return;
            off = j->ppos[p];
            endoff = j->pend[p];
            if (open_part(j, p, off, endoff - 1, err, sizeof(err)) < 0) return;
            j->cretry[j->prun[p]] = 0;
            if (off > 0 && j->http[p].status != 206) {
                http_close(&j->http[p]);
                j->error = 1;
                snprintf(j->err, sizeof(j->err), "server ignored range");
                return;
            }
            if (off > 0 && j->http[p].range_start >= 0 && j->http[p].range_start != off) {
                http_close(&j->http[p]);
                j->error = 1;
                snprintf(j->err, sizeof(j->err), "wrong range %lld != %lld",
                         j->http[p].range_start, off);
                return;
            }
            j->woff[p] = off;
            j->wlen[p] = 0;
            mlog("dl resume part %d at %lld", p, off);
            /* A synchronous connect must not chain more handshakes before reads. */
            return;
        }

        ci = next_pending(j);
        if (ci < 0) continue;

        {
            int run = run_chunks(j);

            endc = ci + 1;
            while (endc < j->nchunks && endc - ci < run &&
                   j->cstate[endc] == 0 && j->cgot[endc] == 0)
                endc++;
        }

        off = chunk_off(j, ci) + j->cgot[ci];
        endoff = (j->total > 0) ? chunk_off(j, endc) : -1;
        if (j->total > 0 && endoff > j->total) endoff = j->total;

        if (open_part(j, p, off, endoff - 1, err, sizeof(err)) < 0) return;
        j->cretry[ci] = 0;
        if (off > 0 && j->http[p].status != 206) {
            http_close(&j->http[p]);
            j->error = 1;
            snprintf(j->err, sizeof(j->err), "server ignored range");
            return;
        }
        if (off > 0 && j->http[p].range_start >= 0 && j->http[p].range_start != off) {
            http_close(&j->http[p]);
            j->error = 1;
            snprintf(j->err, sizeof(j->err), "wrong range %lld != %lld",
                     j->http[p].range_start, off);
            return;
        }

        for (i = ci; i < endc; ++i) {
            j->cstate[i] = 1;
            j->cpart[i] = p;
        }
        j->prun[p] = ci;
        j->prun_end[p] = endc;
        j->ppos[p] = off;
        j->pend[p] = (endoff > 0) ? endoff : 0x7FFFFFFF;
        j->woff[p] = off;
        j->wlen[p] = 0;
        return; /* Pump this connection before opening another range. */
    }
}

static int pump_part(DlJob *j, int p)
{
    int progress = 0;

    if (j->prun_end[p] <= j->prun[p]) return 0;
    if (j->http[p].sock < 0) return 0;

    for (int k = 0; k < 8; ++k) {
        long long remain = j->pend[p] - j->ppos[p];
        int room = DL_WBUF - j->wlen[p];
        int want, n;

        if (remain <= 0) {
            flush_part(j, p);
            part_drop(j, p);
            return 1;
        }
        if (room <= 0) {
            if (flush_part(j, p) < 0) return progress;
            continue;
        }

        want = remain < (long long)room ? (int)remain : room;
        n = http_read(&j->http[p], (char *)g_wbuf[p] + j->wlen[p], want, 0, NULL, -1,
                      j->err, sizeof(j->err));

        if (n > 0) {
            j->wlen[p] += n;
            j->ppos[p] += n;

            /* 攒满一个簇（或这一段读完了）才真正写盘 */
            if (j->wlen[p] >= DL_WBUF || j->ppos[p] >= j->pend[p] ||
                (j->total > 0 && j->ppos[p] >= j->total)) {
                if (flush_part(j, p) < 0) return progress;
            }

            if (j->total > 0 && j->ppos[p] >= j->total) { part_drop(j, p); return 1; }
            if (j->ppos[p] >= j->pend[p]) { part_drop(j, p); return 1; }
            progress = 1;
            continue;
        }

        if (n == 0) {
            flush_part(j, p);
            if (j->ppos[p] >= j->pend[p]) { part_drop(j, p); return 1; }
            if (j->total <= 0) {
                j->cstate[0] = 2;
                j->done_chunks = j->nchunks;
                part_drop(j, p);
                return 1;
            }
             /* 连接提前关掉：下一轮 assign 里从 ppos 续传 */
            part_drop(j, p);
            retry_later(j, p);
            return progress;
        }

        if (n == -1) {
            /* A short receive at the live prefix is playable only after write.
             * Do not wait for a full 32KB cluster while the decoder is hungry. */
            if (j->is_stream && j->wlen[p] > 0 &&
                j->woff[p] == job_prefix(j))
                flush_part(j, p);
            return progress;
        }

        /* 出错：把已经收到的写掉，再断开重连 */
        flush_part(j, p);
        part_drop(j, p);
        retry_later(j, p);
        return progress;
    }
    if (j->is_stream && j->wlen[p] > 0 && j->woff[p] == job_prefix(j))
        flush_part(j, p);
    return progress;
}

static int job_pump(DlJob *j)
{
    int did = 0;
    unsigned int now, live_before_assign = 0;

    if (!j->active || j->complete || j->error) return 0;

    /* Total bytes can keep growing while the playable prefix is stuck.
     * Watch that prefix independently, and retry only its owner at low water. */
    now = sceKernelGetSystemTimeLow();
    if (j->is_stream) {
        long long prefix;
        dl_lock();
        if (g_read_gen == j->gen) j->read_off = g_read_off;
        dl_unlock();
        prefix = job_prefix(j);
        if (!j->prefix_t || prefix != j->watch_prefix) {
            j->watch_prefix = prefix;
            j->prefix_t = now;
        } else if (prefix_low(j) && now - j->prefix_t > 2 * 1000 * 1000) {
            int owner = prefix_part(j);
            if (owner >= 0) {
                if (flush_part(j, owner) < 0) { job_finish(j); return 0; }
                part_drop(j, owner);
            }
            j->prefix_t = now;
        }
    }

    /* Drain live sockets before any synchronous DNS/connect/header wait. */
    for (int p = 0; p < j->nparts; ++p) {
        if (j->http[p].sock < 0) continue;
        if (prefix_low(j) && p != prefix_part(j)) continue;
        if (pump_part(j, p)) did = 1;
        if (j->error) break;
    }
    if (j->error) { job_finish(j); return did; }

    for (int p = 0; p < j->nparts; ++p)
        if (j->http[p].sock >= 0) live_before_assign |= 1u << p;
    assign_parts(j);
    if (j->error) { job_finish(j); return did; }

    /* Read the newly opened range now; old sockets already had their turn. */
    for (int p = 0; p < j->nparts; ++p) {
        if ((live_before_assign & (1u << p)) || j->http[p].sock < 0) continue;
        if (prefix_low(j) && p != prefix_part(j)) continue;
        if (pump_part(j, p)) did = 1;
        if (j->error) break;
    }

    if (j->error) { job_finish(j); return did; }

    /* A sparse file's byte count is not a playable completion proof. */
    if ((j->total > 0 && job_prefix(j) >= j->total) ||
        (j->total <= 0 && j->done_chunks >= j->nchunks)) {
        job_finish(j);
        return 1;
    }

    /* 看门狗：8 秒没有任何进展就把连接全丢了重连（服务端半死/连接卡住） */
    now = sceKernelGetSystemTimeLow();
    if (j->done_bytes != j->watch_bytes) {
        j->watch_bytes = j->done_bytes;
        j->watch_t = now;
        j->stall_n = 0;
    } else if (now - j->watch_t > 8 * 1000 * 1000) {
        j->watch_t = now;
        j->stall_n++;

        mlog("dl stall id=%lld done=%lld/%d stall=%d", j->id, j->done_bytes, j->total, j->stall_n);
        for (int p = 0; p < j->nparts; ++p) {
            if (j->http[p].sock >= 0) http_close(&j->http[p]);
            retry_later(j, p);
        }
    }

    return did;
}
/* 线程                                                                */
/* ------------------------------------------------------------------ */

/* Private queue metadata: URLs stay in the application directory, never logs.
 * Stream one record at a time; a 32-item snapshot exceeds the PSP worker stack.
 * The version includes the native record size so ABI changes reject old data. */
#define QUEUE_PATH APP_DIR "download-queue.bin"
#define QUEUE_TMP  QUEUE_PATH ".tmp"
#define QUEUE_OLD  QUEUE_PATH ".old"

static unsigned int queue_hash(unsigned int h, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static int queue_write(FILE *f, const void *p, size_t n, unsigned int *hash)
{
    if (fwrite(p, 1, n, f) != n) return -1;
    *hash = queue_hash(*hash, p, n);
    return 0;
}

static int queue_read(FILE *f, void *p, size_t n, unsigned int *hash)
{
    if (fread(p, 1, n, f) != n) return -1;
    *hash = queue_hash(*hash, p, n);
    return 0;
}

static void queue_save(void)
{
    unsigned int head[4], hash = 2166136261u;
    int n, gen, ok = 1, had_old = 0;
    FILE *f;
    dl_lock();
    if (!g_queue_dirty) { dl_unlock(); return; }
    n = g_queue_n;
    gen = g_qgen;
    dl_unlock();
    head[0] = 0x514c444du; /* MDLQ */
    head[1] = 1;
    head[2] = sizeof(DlItem);
    head[3] = (unsigned int)n;
    io_lock();
    f = fopen(QUEUE_TMP, "wb");
    if (!f) { io_unlock(); return; }
    if (queue_write(f, head, sizeof(head), &hash) < 0) ok = 0;
    for (int i = 0; ok && i < n; ++i) {
        DlItem item;
        char url[1000];
        unsigned int len;
        dl_lock();
        if (gen != g_qgen || n != g_queue_n) ok = 0;
        if (ok) { item = g_queue[i]; memcpy(url, g_urls[i], sizeof(url)); }
        dl_unlock();
        if (!ok) break;
        len = (unsigned int)strlen(url);
        if (!len || len >= sizeof(url) ||
            queue_write(f, &item, sizeof(item), &hash) < 0 ||
            queue_write(f, &len, sizeof(len), &hash) < 0 ||
            queue_write(f, url, len, &hash) < 0) ok = 0;
    }
    if (ok && fwrite(&hash, 1, sizeof(hash), f) != sizeof(hash)) ok = 0;
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    dl_lock();
    if (gen != g_qgen || n != g_queue_n) ok = 0;
    dl_unlock();
    if (ok) {
        f = fopen(QUEUE_PATH, "rb");
        if (f) { had_old = 1; fclose(f); }
        if (had_old) {
            remove(QUEUE_OLD);
            if (rename(QUEUE_PATH, QUEUE_OLD) != 0) ok = 0;
        }
        if (ok && rename(QUEUE_TMP, QUEUE_PATH) != 0) {
            ok = 0;
            if (had_old) rename(QUEUE_OLD, QUEUE_PATH);
        }
    }
    if (!ok) remove(QUEUE_TMP);
    io_unlock();
    if (ok) {
        dl_lock();
        if (gen == g_qgen && n == g_queue_n) g_queue_dirty = 0;
        dl_unlock();
    }
}

/* Initialization only: publish count after the complete bounded file validates. */
static int queue_read_file(const char *path)
{
    unsigned int head[4], hash = 2166136261u, saved;
    long length;
    int ok = 1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < (long)(sizeof(head) + sizeof(saved)) ||
        length > (long)(sizeof(head) + DL_MAX_QUEUE * (sizeof(DlItem) + sizeof(unsigned int) + 999) + sizeof(saved)) ||
        fseek(f, 0, SEEK_SET) != 0 || queue_read(f, head, sizeof(head), &hash) < 0 ||
        head[0] != 0x514c444du || head[1] != 1 || head[2] != sizeof(DlItem) || head[3] > DL_MAX_QUEUE) ok = 0;
    for (unsigned int i = 0; ok && i < head[3]; ++i) {
        DlItem *it = &g_queue[i];
        unsigned int len;
        if (queue_read(f, it, sizeof(*it), &hash) < 0 ||
            queue_read(f, &len, sizeof(len), &hash) < 0 || !len || len >= sizeof(g_urls[i]) ||
            queue_read(f, g_urls[i], len, &hash) < 0) { ok = 0; break; }
        g_urls[i][len] = 0;
        if (memchr(g_urls[i], 0, len) || it->id <= 0 || it->song.id != it->id ||
            it->total < 0 || it->pct < 0 || it->pct > 100 ||
            (it->state != DL_PENDING && it->state != DL_ACTIVE && it->state != DL_PAUSED) ||
            !memchr(it->song.name, 0, sizeof(it->song.name)) ||
            !memchr(it->song.artist, 0, sizeof(it->song.artist)) ||
            !memchr(it->song.album, 0, sizeof(it->song.album)) ||
            !memchr(it->song.art, 0, sizeof(it->song.art))) ok = 0;
        for (unsigned int k = 0; k < i; ++k) if (g_queue[k].id == it->id) ok = 0;
        it->state = DL_PAUSED;
    }
    if (ok && (fread(&saved, 1, sizeof(saved), f) != sizeof(saved) || saved != hash || fgetc(f) != EOF)) ok = 0;
    if (ferror(f)) ok = 0;
    fclose(f);
    if (!ok) return -1;
    g_queue_n = (int)head[3];
    return 0;
}

static void queue_load(void)
{
    g_queue_n = 0;
    io_lock();
    if (queue_read_file(QUEUE_PATH) < 0 && queue_read_file(QUEUE_OLD) < 0) g_queue_n = 0;
    io_unlock();
    g_queue_dirty = 0;
    /* A completed cache may have been published just before the old queue
     * snapshot reached disk. Never resume it through a second writer/TRUNC.
     * cache_has can acquire filesystem locks, so filter outside io_lock. */
    for (int i = 0; i < g_queue_n;) {
        if (!cache_has(g_queue[i].id)) { ++i; continue; }
        for (int k = i; k + 1 < g_queue_n; ++k) {
            g_queue[k] = g_queue[k + 1];
            memcpy(g_urls[k], g_urls[k + 1], sizeof(g_urls[k]));
        }
        --g_queue_n;
        g_queue_dirty = 1;
    }
    g_qgen++;
}

static void log_speed(DlJob *j)
{
    unsigned int now = sceKernelGetSystemTimeLow();

    if (now - j->t_log < 4 * 1000 * 1000) return;
    j->t_log = now;

    {
        int ms = (int)((now - j->t0) / 1000);
        int kbps = ms > 0 ? (int)(j->done_bytes / 1024 * 1000 / ms) : 0;
        char parts[64] = "";
        int off = 0;

        g_speed = kbps;
        for (int p = 0; p < j->nparts && off < (int)sizeof(parts) - 12; ++p) {
            off += snprintf(parts + off, sizeof(parts) - (size_t)off, "%s%d",
                            p ? " " : "", (int)(j->pbytes[p] / 1024));
            j->pbytes[p] = 0;
        }
        mlog("dl %s id=%lld %d/%dKB %dKB/s parts=%d [%s]",
             j->is_stream ? "stream" : "queue",
             j->id, (int)j->done_bytes / 1024, j->total / 1024, kbps, j->nparts, parts);
    }
}

static void suspend_queue(void)
{
    int live = 0;
    if (!g_qjob.active) return;
    for (int p = 0; p < DL_MAX_PARTS; ++p) {
        if (g_qjob.http[p].sock >= 0 || g_qjob.wlen[p] > 0) live = 1;
        if (g_qjob.wlen[p] > 0) flush_part(&g_qjob, p);
    }
    if (!live) return;
    job_close_net(&g_qjob);
    /* Keep the file and the chunk map: resuming must not call job_begin/TRUNC. */
    mlog("dl queue suspended");
}

static void queue_done(void)
{
    dl_lock();
    if (g_queue_n > 0) {
        DlItem *it = &g_queue[0];

        it->state = g_qjob.error ? DL_FAILED : DL_DONE;
        /* A failure keeps whatever prefix was verified so the row still explains
         * how far it got; only a published completion is 100%. */
        it->pct = g_qjob.error ? dl_pct_from_bytes(g_qjob.done_bytes, g_qjob.total, 0) : 100;
        g_qgen++; g_queue_dirty = 1;

        for (int i = 0; i + 1 < g_queue_n; ++i) {
            g_queue[i] = g_queue[i + 1];
            memcpy(g_urls[i], g_urls[i + 1], sizeof(g_urls[i]));
        }
        g_queue_n--;
    }
    dl_unlock();

    job_reset(&g_qjob);
}

/* The two jobs alternate on one worker and share one write-buffer bank.
 * No received bytes may survive a hand-off in that shared bank. */
static int pump_job(DlJob *j)
{
    int did = job_pump(j);
    for (int p = 0; p < DL_MAX_PARTS; ++p)
        if (j->wlen[p] > 0) flush_part(j, p);
    if (j->error && j->active) job_finish(j);
    if (j->active && (job_prefix(j) - j->saved_prefix >= 512 * 1024 ||
        sceKernelGetSystemTimeLow() - j->checkpoint_t > 2000000u)) checkpoint_save(j);
    return did;
}

static void remove_queued_id(long long id)
{
    dl_lock();
    for (int i = 0; i < g_queue_n; ++i) {
        if (g_queue[i].id != id) continue;
        for (int k = i; k + 1 < g_queue_n; ++k) {
            g_queue[k] = g_queue[k + 1];
            memcpy(g_urls[k], g_urls[k + 1], sizeof(g_urls[k]));
        }
        g_queue_n--;
        g_qgen++; g_queue_dirty = 1;
        break;
    }
    dl_unlock();
}

static void release_stream(void)
{
    for (int p = 0; p < DL_MAX_PARTS; ++p)
        if (g_stream.wlen[p] > 0) flush_part(&g_stream, p);
    job_close_net(&g_stream);
    job_close_file(&g_stream);
    if (g_stream.keep_queued && !g_stream.complete && !g_stream.error) {
        /* The promoted item reserves one capacity slot until completion. */
        dl_lock();
        if (g_queue_n < DL_MAX_QUEUE) {
            int i = g_queue_n++;
            memset(&g_queue[i], 0, sizeof(g_queue[i]));
            g_queue[i].id = g_stream.id;
            g_queue[i].song = g_stream.song;
            g_queue[i].total = g_stream.total;
            g_queue[i].state = DL_PENDING;
            /* Promote the bytes already on disk instead of restarting the row at 0%. */
            g_queue[i].pct = dl_pct_from_bytes(g_stream.done_bytes, g_stream.total, 0);
            snprintf(g_urls[i], sizeof(g_urls[i]), "%s", g_stream.url);
            g_qgen++; g_queue_dirty = 1;
        }
        dl_unlock();
    }
    g_stream.keep_queued = 0;
}

static void start_stream(const Song *s, const char *url, int total, int gen)
{
    int queued = 0;
    release_stream();
    dl_lock();
    for (int i = 0; i < g_queue_n; ++i)
        if (g_queue[i].id == s->id) queued = 1;
    dl_unlock();
    if (g_qjob.active && g_qjob.id == s->id &&
        (total <= 0 || g_qjob.total == total)) {
        /* Transfer the sole fd and written chunk map, never open a second writer.
         * This also preserves valid progress if playback is requested mid-queue. */
        for (int p = 0; p < DL_MAX_PARTS; ++p)
            if (g_qjob.wlen[p] > 0) flush_part(&g_qjob, p);
        job_close_net(&g_qjob);
        g_stream = g_qjob;
        job_reset(&g_qjob);
        g_stream.is_stream = 1;
        g_stream.nparts = g_stream.total <= 0 ? 1 : g_parts;
        if (g_stream.nparts > g_stream.nchunks) g_stream.nparts = g_stream.nchunks;
        g_stream.gen = gen;
        g_stream.read_off = 0;
        g_stream.prefix_t = 0;
        g_stream.next_open_at = 0;
        memset(g_stream.retry_at, 0, sizeof(g_stream.retry_at));
        memset(g_stream.retry_ms, 0, sizeof(g_stream.retry_ms));
        g_stream.checkpoint_dirty = strcmp(g_stream.url, url) != 0;
        snprintf(g_stream.url, sizeof(g_stream.url), "%s", url);
    } else {
        if (g_qjob.id == s->id && g_qjob.fd >= 0) {
            for (int p = 0; p < DL_MAX_PARTS; ++p)
                if (g_qjob.wlen[p] > 0) flush_part(&g_qjob, p);
            job_close_net(&g_qjob);
            job_close_file(&g_qjob);
            job_reset(&g_qjob);
        }
        job_begin(&g_stream, s, url, total, 1, gen);
    }
    g_stream.keep_queued = queued;
    remove_queued_id(s->id);
    if (!g_stream.active)
        mlog("dl stream: begin failed id=%lld err=%s", s->id,
             g_stream.err[0] ? g_stream.err : "inactive");
}

static void queue_pause_requested(void)
{
    long long id;
    int pct = -1;
    dl_lock();
    id = g_cmd_qpause_id;
    g_cmd_qpause_id = 0;
    dl_unlock();
    if (!id) return;
    if (g_qjob.id == id && g_qjob.active) {
        for (int p = 0; p < DL_MAX_PARTS; ++p)
            if (g_qjob.wlen[p] > 0) flush_part(&g_qjob, p);
        if (g_qjob.total > 0) pct = dl_pct_from_bytes(g_qjob.done_bytes, g_qjob.total, 0);
        job_close_net(&g_qjob);
        job_close_file(&g_qjob);
        job_reset(&g_qjob);
    }
    dl_lock();
    for (int i = 0; i < g_queue_n; ++i) {
        if (g_queue[i].id != id) continue;
        g_queue[i].state = DL_PAUSED;
        if (pct >= 0) g_queue[i].pct = pct;
        g_qgen++; g_queue_dirty = 1;
        g_queue_dirty = 1;
        break;
    }
    dl_unlock();
}

static void queue_start_next(void)
{
    DlItem item;
    char url[1000];
    int at = -1;
    if (g_qjob.active) return;
    dl_lock();
    for (int i = 0; i < g_queue_n; ++i)
        if (g_queue[i].state == DL_PENDING) { at = i; break; }
    if (at < 0) { dl_unlock(); return; }
    item = g_queue[at];
    memcpy(url, g_urls[at], sizeof(url));
    for (int i = at; i > 0; --i) {
        g_queue[i] = g_queue[i - 1];
        memcpy(g_urls[i], g_urls[i - 1], sizeof(g_urls[i]));
    }
    g_queue[0] = item;
    memcpy(g_urls[0], url, sizeof(g_urls[0]));
    g_queue[0].state = DL_ACTIVE;
    g_qgen++; g_queue_dirty = 1;
    g_queue_dirty = 1;
    dl_unlock();
    if (job_begin(&g_qjob, &item.song, url, item.total, 0, 0) < 0) queue_done();
}

static int dl_thread(SceSize args, void *argp)
{
#if defined(__PSP__)
    melody_bind_thread_directory();
#endif

    (void)args;
    (void)argp;

    while (!g_quit) {
        int did = 0;

         /* 处理播放流的开始 / 停止命令（只有本线程能改 g_stream） */
        {
            int do_start = 0, do_stop = 0, total = 0, gen = 0;
            Song s;
            char url[1000];

            memset(&s, 0, sizeof(s));
            url[0] = 0;

            dl_lock();
            if (g_cmd_pending) {
                do_start = 1;
                s = g_cmd_song;
                snprintf(url, sizeof(url), "%s", g_cmd_url);
                total = g_cmd_total;
                gen = g_cmd_gen;
                g_cmd_pending = 0;
                g_cmd_stop = 0;
            } else if (g_cmd_stop) {
                do_stop = 1;
                g_cmd_stop = 0;
            }
            dl_unlock();

            if (do_start) {
                start_stream(&s, url, total, gen);
            } else if (do_stop) {
                release_stream();
                g_stream.active = 0;
                g_stream.complete = 0;
                g_stream.error = 0;
                g_stream.gen = 0;
            }
        }

         /* 队列的取消 / 清空也交给本线程处理，避免两个线程同时碰 g_qjob */
        if (g_cmd_qclear) {
            g_cmd_qclear = 0;
            for (int p = 0; p < DL_MAX_PARTS; ++p)
                if (g_qjob.wlen[p] > 0) flush_part(&g_qjob, p);
            job_close_net(&g_qjob);
            job_close_file(&g_qjob);
            job_reset(&g_qjob);
            dl_lock();
            g_queue_n = 0;
            g_stream.keep_queued = 0;
            g_qgen++; g_queue_dirty = 1;
            dl_unlock();
            mlog("dl queue cleared");
        }
        if (g_cmd_qcancel) {
            long long id;
            dl_lock();
            id = g_cmd_qcancel_id;
            g_cmd_qcancel = 0;
            g_cmd_qcancel_id = 0;
            dl_unlock();
            if (g_qjob.active && g_qjob.id == id) {
                for (int p = 0; p < DL_MAX_PARTS; ++p)
                    if (g_qjob.wlen[p] > 0) flush_part(&g_qjob, p);
                job_close_net(&g_qjob);
                job_close_file(&g_qjob);
                job_reset(&g_qjob);
            }
            remove_queued_id(id);
        }

        queue_pause_requested();
        queue_save();
        if(g_video_priority){suspend_queue();g_speed=0;sceKernelDelayThread(20000);continue;}
        if (!g_stream.active && !g_qjob.active) g_speed = 0;

        if (g_stream.active && !g_stream.complete) {
            did = pump_job(&g_stream);
            if (g_stream.active) log_speed(&g_stream);
        }
        if (g_stream.active && !g_stream.complete && !g_stream.error && prefix_low(&g_stream)) {
            suspend_queue();
        } else if (g_queue_n > 0) {
            queue_start_next();

            if (g_qjob.active) {
                int parts = g_stream.active ? 1 : g_parts;
                if (parts > g_qjob.nchunks) parts = g_qjob.nchunks;
                if (parts < 1) parts = 1;
                if (g_qjob.total <= 0) parts = 1;
                /* NET_POOL_SIZE also holds API/cover sockets. Do not multiply
                 * two six-part jobs into twelve 64KB TCP receive windows. */
                if (parts < g_qjob.nparts) suspend_queue();
                g_qjob.nparts = parts;
                did |= pump_job(&g_qjob);
                if (g_qjob.active) {
                    if (g_qjob.total > 0) {
                        int pct = dl_pct_from_bytes(g_qjob.done_bytes, g_qjob.total, 0);
                        dl_lock();
                        if (g_queue_n > 0 && g_queue[0].id == g_qjob.id && g_queue[0].pct != pct) {
                            g_queue[0].pct = pct;
                            g_queue_dirty = 1;
                        }
                        dl_unlock();
                    }
                    log_speed(&g_qjob);
                }
                if (!g_qjob.active) queue_done();
            }
        }

        if (!did) sceKernelDelayThread(2000);
    }

    for (int p = 0; p < DL_MAX_PARTS; ++p) {
        if (g_stream.wlen[p] > 0) flush_part(&g_stream, p);
        if (g_qjob.wlen[p] > 0) flush_part(&g_qjob, p);
    }
    job_close_net(&g_stream);
    job_close_file(&g_stream);
    job_close_net(&g_qjob);
    job_close_file(&g_qjob);
    queue_save();
    return 0;
}

/* ------------------------------------------------------------------ */
 /* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int dl_stream_start(const Song *s, const char *url, int total, int gen)
{
    if (!s || !url || !url[0]) return -1;

    dl_lock();
    g_cmd_song = *s;
    snprintf(g_cmd_url, sizeof(g_cmd_url), "%s", url);
    g_cmd_total = total;
    g_cmd_gen = gen;
    g_cmd_pending = 1;
    g_cmd_stop = 0;
    g_io_epoch++;
    dl_unlock();

    return 0;
}

void dl_stream_stop(void)
{
    dl_lock();
    g_cmd_stop = 1;
    g_cmd_pending = 0;
    g_io_epoch++;
    dl_unlock();
}

int dl_stream_active(void) { return g_stream.active; }

long long dl_stream_available(int gen)
{
    DlJob *j = &g_stream;
    if (j->gen != gen) return 0;

    /* 下载已经完成：整首都在文件里，可读前缀就是全长。
     * 以前这里只看 active，下载一完成就返回 0，
     * 表现就是「缓存完就停播，快进还提示无数据」。 */
    if (j->complete) return (j->total > 0) ? (long long)j->total : j->done_bytes;
    if (!j->active) return 0;

    return job_prefix(j);
}

int dl_stream_readable(int gen, long long off, int want)
{
    DlJob *j = &g_stream;
    long long available;

    if (off < 0 || off > 0x7FFFFFFF || want <= 0) return 0;
    dl_lock();
    if (j->gen != gen || g_cmd_stop ||
        (g_cmd_pending && g_cmd_gen != gen)) {
        dl_unlock();
        return 0;
    }
    /* Publish a tagged command, never write into a job being reset by dl. */
    if (g_read_gen != gen) {
        g_read_gen = gen;
        g_read_off = (int)off;
    } else if (off > g_read_off) g_read_off = (int)off;
    if (g_open_background && job_prefix(j) - off < DL_PREBUF) g_io_epoch++;
    dl_unlock();
    available = dl_stream_available(gen);
    if (available <= off) return 0;
    available -= off;
    return available < want ? (int)available : want;
}

int dl_stream_total(int gen)
{
    if (g_stream.gen != gen) return 0;
    return g_stream.total;
}

int dl_stream_done(int gen)
{
    if (g_stream.gen != gen) return 0;
    return g_stream.complete;
}

int dl_stream_failed(int gen)
{
    if (g_stream.gen != gen) return 0;
    return g_stream.error;
}

const char *dl_stream_error(void) { return g_stream.err; }
long long dl_stream_bytes(void) { return g_stream.done_bytes; }

int dl_has_song(long long id)
{
    int found = cache_has(id);
    dl_lock();
    if ((g_cmd_pending && g_cmd_song.id == id) ||
        ((g_stream.active || g_stream.complete) && !g_cmd_stop && g_stream.id == id))
        found = 1;
    for (int i = 0; !found && i < g_queue_n; ++i)
        if (g_queue[i].id == id) found = 1;
    dl_unlock();
    return found;
}

int dl_enqueue(const Song *s, const char *url, int total)
{
    int i;

    if (!s || !url || !url[0] || strlen(url) >= sizeof(g_urls[0])) return -1;
    if (cache_has(s->id)) return -2;

    dl_lock();

    if ((g_cmd_pending && g_cmd_song.id == s->id) ||
        ((g_stream.active || g_stream.complete) && !g_cmd_stop && g_stream.id == s->id)) {
        dl_unlock(); return -3;
    }
    if (g_queue_n + (g_stream.keep_queued && !g_stream.complete ? 1 : 0) >= DL_MAX_QUEUE) { dl_unlock(); return -1; }

    for (i = 0; i < g_queue_n; ++i) {
        if (g_queue[i].id == s->id) { dl_unlock(); return -3; }
    }

    i = g_queue_n++;
    memset(&g_queue[i], 0, sizeof(g_queue[i]));
    g_queue[i].id = s->id;
    g_queue[i].song = *s;
    g_queue[i].total = total;
    g_queue[i].state = DL_PENDING;
    snprintf(g_urls[i], sizeof(g_urls[i]), "%s", url);
    g_qgen++; g_queue_dirty = 1;

    dl_unlock();

    mlog("dl enqueue %lld %s", s->id, s->name);
    return i;
}

int dl_queue_count(void) { return g_queue_n; }

const DlItem *dl_queue_at(int i)
{
    if (i < 0 || i >= g_queue_n) return NULL;
    return &g_queue[i];
}

int dl_queue_snapshot(DlItem *out, int max)
{
    int n;

    dl_lock();
    n = g_queue_n < max ? g_queue_n : max;
    for (int i = 0; i < n; ++i) out[i] = g_queue[i];
    dl_unlock();
    return n;
}

int dl_queue_pause(int index)
{
    int result = -1;
    dl_lock();
    if (index >= 0 && index < g_queue_n) {
        DlItem *it = &g_queue[index];
        if (it->state == DL_PENDING) {
            it->state = DL_PAUSED;
            g_qgen++; g_queue_dirty = 1;
            g_queue_dirty = 1;
            result = 0;
        } else if (it->state == DL_ACTIVE && !g_cmd_qpause_id) {
            g_cmd_qpause_id = it->id;
            result = 0;
        }
    }
    dl_unlock();
    return result;
}

int dl_queue_resume(int index)
{
    int result = -1;
    dl_lock();
    if (index >= 0 && index < g_queue_n &&
        g_queue[index].state == DL_PAUSED && g_urls[index][0]) {
        g_queue[index].state = DL_PENDING;
        g_queue_dirty = 1;
        g_qgen++; g_queue_dirty = 1;
        result = 0;
    }
    dl_unlock();
    return result;
}

void dl_queue_cancel(int index)
{
    dl_lock();
    if (index < 0 || index >= g_queue_n) { dl_unlock(); return; }
    if (index == 0) {
        g_cmd_qcancel_id = g_queue[index].id;
        g_cmd_qcancel = 1;
        dl_unlock();
        return;
    }
    for (int i = index; i + 1 < g_queue_n; ++i) {
        g_queue[i] = g_queue[i + 1];
        memcpy(g_urls[i], g_urls[i + 1], sizeof(g_urls[i]));
    }
    g_queue_n--;
    g_qgen++; g_queue_dirty = 1;
    dl_unlock();
    mlog("dl cancel %d", index);
}

void dl_queue_clear(void)
{
    g_cmd_qclear = 1;
    g_io_epoch++;
}

int dl_queue_gen(void) { return g_qgen; }
int dl_speed_kbps(void) { return g_speed; }

void dl_set_parts(int n)
{
    if (n < 1) n = 1;
    if (n > DL_MAX_PARTS) n = DL_MAX_PARTS;
    g_parts = n;
    mlog("dl parts = %d", g_parts);
}

int dl_get_parts(void) { return g_parts; }

/* ------------------------------------------------------------------ */
 /* 初始化                                                              */
/* ------------------------------------------------------------------ */

void dl_init(void)
{
    job_reset(&g_stream);
    job_reset(&g_qjob);
    g_queue_n = 0;
    g_quit = 0;

    if (g_lock < 0) g_lock = sceKernelCreateSema("melody_dl_cmd", 0, 1, 1, NULL);
    g_cmd_qpause_id = 0;
    g_cmd_qcancel_id = 0;
    g_cmd_qcancel = g_cmd_qclear = 0;
    queue_load();

    g_thid = sceKernelCreateThread("melody_dl", dl_thread, DL_PRIO, DL_STACK, 0, NULL);
    if (g_thid >= 0) {
        sceKernelStartThread(g_thid, 0, NULL);
        mlog("dl thread started");
    } else {
        mlog("dl thread create failed 0x%08X", g_thid);
    }
}

int dl_shutdown(void)
{
    SceUInt timeout = 300 * 1000;
    int r;

    g_quit = 1;
    g_io_epoch++;
    if (g_thid < 0) return 0;

    r = sceKernelWaitThreadEnd(g_thid, &timeout);
    if (r < 0) {
        mlog("dl thread did not exit in 300ms (0x%08X), terminate", r);
        sceKernelTerminateDeleteThread(g_thid);
        g_thid = -1;
        return 1;
    }
    sceKernelDeleteThread(g_thid);
    g_thid = -1;
    return 0;
}

