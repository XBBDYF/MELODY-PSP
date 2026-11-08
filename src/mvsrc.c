#include "thread_paths.h"
/*
 * MELODY PSP — MV 边下边播数据源（P0）
 *
 * 设计要点（与 docs 里的 P0 范围一致）：
 *   1. mvsrc_open() 同步 probe：http_open_bulk() 拿到 Content-Length，>0 才继续。
 *      连接直接交给下载线程继续读，不重复建连（省一次 TCP/TLS 之外的握手）。
 *   2. 下载线程顺序写 <final>.part：写 fd 常开、16KB 缓冲、每块让出 500us，
 *      avail 只增不减。PSP 没有 pread，读侧用 sceIoLseek + sceIoRead，
 *      并用一把信号量串行化（视频和音频线程会并发调用 read_at）。
 *   3. read_at：命中 [0,avail) 直接读盘；否则每 50ms 看一次 avail，
 *      超过 MVSRC_READ_MS 返回 -1。P0 不做 Range 重启，seek 未启用
 *      （interrupt_reads = NULL），size 仍用 probe 到的真实值。
 *   4. 失败回退：probe 失败、HTTP 中途断链、写盘/改名失败 → err=1 并返回错误，
 *      调用方（api.c）走原有整包下载；.part 保留，api.c 的 Range 续传可接着用。
 *
 * 注意：Http 约 16.4KB，必须静态存放。PSP 线程栈只有 32KB，
 *       http_connect_raw 内部还有 req[2600] + hdr[] + cur[1200] 等栈上缓冲。
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "mvsrc.h"
#include "net.h"
#include "log.h"

#define MVSRC_BUF          16384
#define MVSRC_STACK        0x8000        /* 32KB：Http 在静态区，这里留足 net.c 的栈 */
#define MVSRC_PRIO         0x28          /* 低于 mv_play(0x17)，高于主线程 */
#define MVSRC_OPEN_MS      15000
#define MVSRC_IDLE_MS      1000
#define MVSRC_STALL_US     10000000u     /* 10 秒没有任何新字节 = 死链 */
#define MVSRC_READ_MS      10000         /* read_at 单次最长等待 */
#define MVSRC_READ_POLL_US 50000         /* 等 avail 的粒度：50ms */
#define MVSRC_CLOSE_MS     3000
#define MVSRC_MAX_SIZE     (3LL * 1024 * 1024 * 1024)

typedef struct {
    char url[1000];
    char part[160];
    char final[160];
    long long size;               /* probe 到的总长，>0 */
    volatile unsigned avail;      /* [0,avail) 已连续落盘；32 位访问在 MIPS 上是原子的 */
    volatile int done;            /* 已改名成 final */
    volatile int err;             /* 致命错误 */
    volatile int cancel;          /* 永久取消（也当 http_read 的 gen 用） */
    volatile int thread_done;     /* 线程已退出（留给 close 回收） */
    SceUID thid;
    SceUID wfd;                   /* 写 fd，常开 */
    SceUID rfd;                   /* 读 fd，常开；避免每帧 open/close 与改名竞态 */
    SceUID rlock;                 /* 串行化 lseek+read（视频/音频并发） */
    Http http;                    /* 必须静态：16KB+ */
    int http_live;
} MvSrcState;

static MvSrcState g_src;
static unsigned char g_buf[MVSRC_BUF];
static PspVideoSource g_source;
static int g_open;
static int g_inited;    /* g_src 里的 -1 默认值已就位（静态区初值是 0，不是 -1） */

/* ------------------------------------------------------------------ */
/* P1 预留：Range 重启 / seek（P0 只留结构，不启用）                    */
/* ------------------------------------------------------------------ */
/* static int mvsrc_restart(MvSrcState *s, long long off);
 *   1) 现在 cancel 同时兼作 http_read 的 gen；P1 要拆出独立的 gen 字段，
 *      seek 只 gen++ 打断在途读，不动 cancel。
 *   2) 关掉 g_src.http，用 http_open_range(url, off, -1) 重开（off>0 时
 *      net.c 不会走 keep-alive 池，拿到的是独占 64KB 连接），
 *      校验 status==206 && range_start==off；200 只允许 off==0。
 *   3) 写 fd 用 sceIoLseek 定位到 off 再写；用 seg_start/seg_end 取代单一
 *      avail，读侧命中洞（[avail,off)）才允许重启，跨洞读一律拒绝。
 *   4) g_source.interrupt_reads = mvsrc_interrupt；里面 gen++ 并唤醒读线程，
 *      (0) 时清标志。届时再把 mvsrc_source() 里的 NULL 换掉。
 * 现在线程里的 -3 分支只会由 cancel 触发，就是留给这一步的落点。
 */

static int mvsrc_thread(SceSize args, void *argp);
static int mvsrc_read_at(void *opaque, int64_t off, void *buf, int bytes);
static void mvsrc_do_cancel(void *opaque);
static int mvsrc_open_rfd(void);

/* ------------------------------------------------------------------ */
/* 内部：清理                                                          */
/* ------------------------------------------------------------------ */

static void mvsrc_reset_state(void)
{
    memset(&g_src, 0, sizeof(g_src));
    g_src.thid = -1;
    g_src.wfd = -1;
    g_src.rfd = -1;
    g_src.rlock = -1;
    memset(&g_source, 0, sizeof(g_source));
    g_open = 0;
    g_inited = 1;
}

/* 只关资源。线程必须已经退出（或从未启动）才能调用。 */
static void mvsrc_release(void)
{
    if (g_src.http_live) {
        http_close(&g_src.http);
        g_src.http_live = 0;
    }
    if (g_src.wfd >= 0) {
        sceIoClose(g_src.wfd);
        g_src.wfd = -1;
    }
    if (g_src.rfd >= 0) {
        sceIoClose(g_src.rfd);
        g_src.rfd = -1;
    }
    if (g_src.rlock >= 0) {
        sceKernelDeleteSema(g_src.rlock);
        g_src.rlock = -1;
    }
}

/* 下载完成：sync → 关写 fd → part 改名 final。失败置 err。 */
static void mvsrc_finish(void)
{
    if (g_src.wfd >= 0) {
        sceIoSync("ms0:", 0);
        sceIoClose(g_src.wfd);
        g_src.wfd = -1;
    }
    sceIoRemove(g_src.final);
    if (sceIoRename(g_src.part, g_src.final) == 0) {
        g_src.done = 1;
        mlog("mvsrc: done %lld bytes -> %s", (long long)g_src.size, g_src.final);
    } else {
        g_src.err = 1;
        mlog("mvsrc: rename failed %s", g_src.part);
    }
}

/* ------------------------------------------------------------------ */
/* 下载线程                                                            */
/* ------------------------------------------------------------------ */

static int mvsrc_thread(SceSize args, void *argp)
{
#if defined(__PSP__)
    melody_bind_thread_directory();
#endif

    char err[80] = "";
    unsigned last_us = sceKernelGetSystemTimeLow();

    (void)args;
    (void)argp;

    for (;;) {
        int n;

        if (g_src.cancel) break;

        /* gen 传 &cancel、my_gen 传 0：一旦 cancel=1，http_read 立刻返回 -3，
         * 不必等满 idle_ms，MV 退出时不会把线程挂在 recv 上。 */
        n = http_read(&g_src.http, (char *)g_buf, MVSRC_BUF, MVSRC_IDLE_MS,
                      &g_src.cancel, 0, err, sizeof(err));

        if (n > 0) {
            if (sceIoWrite(g_src.wfd, g_buf, n) != n) {
                mlog("mvsrc: write failed avail=%u", (unsigned)g_src.avail);
                g_src.err = 1;
                break;
            }
            g_src.avail += (unsigned)n;
            last_us = sceKernelGetSystemTimeLow();
            if ((long long)g_src.avail >= g_src.size) {
                mvsrc_finish();
                break;
            }
            /* 让出 CPU：解码和呈现要用；16KB/0.5ms 对 11MB 只多花 0.35 秒 */
            sceKernelDelayThread(500);
            continue;
        }

        if (n == 0) {
            /* 提前 EOF：长度没读满就是服务器/链路截断，不能当成功 */
            if ((long long)g_src.avail >= g_src.size) mvsrc_finish();
            else {
                mlog("mvsrc: short eof %u/%lld", (unsigned)g_src.avail, g_src.size);
                g_src.err = 1;
            }
            break;
        }

        if (n == -1) {
            /* 暂时没数据：只要还在推进就不算错，超过 10 秒没有新字节才失败 */
            if ((unsigned)(sceKernelGetSystemTimeLow() - last_us) > MVSRC_STALL_US) {
                mlog("mvsrc: stall at %u/%lld", (unsigned)g_src.avail, g_src.size);
                g_src.err = 1;
                break;
            }
            continue;
        }

        if (n == -3) {
            /* P0 没有 Range 重启；-3 只可能来自 cancel */
            if (g_src.cancel) break;
            mlog("mvsrc: read aborted");
            g_src.err = 1;
            break;
        }

        /* n == -2：连接坏了。P0 不续传（P1 用 Range 从 avail 重启），
         * 直接失败交给调用方整包下载，.part 会被 api.c 的 Range 续传接走。 */
        mlog("mvsrc: read error (%s) at %u/%lld", err, (unsigned)g_src.avail, g_src.size);
        g_src.err = 1;
        break;
    }

    if (g_src.wfd >= 0) {
        sceIoClose(g_src.wfd);
        g_src.wfd = -1;
    }
    g_src.thread_done = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 读侧                                                                */
/* ------------------------------------------------------------------ */

static int mvsrc_open_rfd(void)
{
    if (g_src.rfd >= 0) return 0;
    g_src.rfd = sceIoOpen(g_src.part, PSP_O_RDONLY, 0);
    if (g_src.rfd < 0) g_src.rfd = sceIoOpen(g_src.final, PSP_O_RDONLY, 0);
    return g_src.rfd >= 0 ? 0 : -1;
}

static int mvsrc_read_at(void *opaque, int64_t off, void *buf, int bytes)
{
    MvSrcState *s = opaque ? (MvSrcState *)opaque : &g_src;
    unsigned t0;
    int n;

    if (!g_open || !buf || off < 0 || bytes <= 0) return -1;
    if (s->size > 0 && off >= s->size) return 0;                 /* EOF */
    if (s->size > 0 && (int64_t)bytes > s->size - off)
        bytes = (int)(s->size - off);
    if (bytes <= 0) return 0;

    t0 = sceKernelGetSystemTimeLow();
    for (;;) {
        if (s->cancel || s->err) return -1;
        if ((long long)s->avail >= off + bytes) break;           /* 数据够了 */
        if (s->done) break;                                      /* 不会再长：能读多少读多少 */
        if ((unsigned)(sceKernelGetSystemTimeLow() - t0) / 1000u > (unsigned)MVSRC_READ_MS) {
            mlog("mvsrc: read timeout off=%lld need=%d avail=%u",
                 (long long)off, bytes, (unsigned)s->avail);
            return -1;
        }
        sceKernelDelayThread(MVSRC_READ_POLL_US);
    }

    if (mvsrc_open_rfd() < 0) return -1;

    /* 视频和音频两个线程会同时进来：lseek+read 必须成对串行 */
    {
        SceUInt tmo = 2000000;                                   /* 2 秒：另一个读者不会占这么久 */
        if (sceKernelWaitSema(s->rlock, 1, &tmo) < 0) return -1;
    }

    n = -1;
    if (sceIoLseek(s->rfd, (SceOff)off, PSP_SEEK_SET) == (SceOff)off)
        n = sceIoRead(s->rfd, buf, bytes);

    sceKernelSignalSema(s->rlock, 1);
    return n < 0 ? -1 : n;
}

static void mvsrc_do_cancel(void *opaque)
{
    (void)opaque;
    g_src.cancel = 1;
}

/* ------------------------------------------------------------------ */
/* 对外 API                                                            */
/* ------------------------------------------------------------------ */

int mvsrc_open(const char *url, const char *final_path)
{
    SceIoStat st;
    char err[96] = "";
    int n;

    if (!url || !url[0] || !final_path || !final_path[0]) return -1;

    /* 重入：先把上一轮收干净（调用方忘了 close 也不会留下僵尸线程/fd） */
    if (g_open || g_src.thid >= 0) mvsrc_close();

    if (strlen(url) >= sizeof(g_src.url)) return -1;
    if (strlen(final_path) >= sizeof(g_src.final)) return -1;

    mvsrc_reset_state();

    /* 本地已有整包：不该走流式（调用方应直接 mv_start 本地播放） */
    if (sceIoGetstat(final_path, &st) >= 0 && st.st_size > 0) {
        mlog("mvsrc: final exists, use local playback");
        return -1;
    }

    memcpy(g_src.url, url, strlen(url) + 1);
    memcpy(g_src.final, final_path, strlen(final_path) + 1);
    n = snprintf(g_src.part, sizeof(g_src.part), "%s.part", final_path);
    if (n < 0 || n >= (int)sizeof(g_src.part)) {
        mlog("mvsrc: part path too long");
        return -1;
    }

    /* P0 不能 Range 续传：遗留的 .part 交给 api.c 的整包下载去续，
     * 这里直接失败，避免把它截断重下。 */
    if (sceIoGetstat(g_src.part, &st) >= 0 && st.st_size > 0) {
        mlog("mvsrc: stale part %lld bytes, fall back to full download",
             (long long)st.st_size);
        return -1;
    }

    /* probe：整包 GET（bulk 连接：不进 keep-alive 池、64KB 接收窗）。
     * 这一步同时把连接留在 g_src.http 里，下载线程直接接着读。 */
    if (http_open_bulk(&g_src.http, url, MVSRC_OPEN_MS, err, sizeof(err)) < 0) {
        mlog("mvsrc: probe failed (%s)", err);
        return -1;
    }
    g_src.http_live = 1;

    g_src.size = g_src.http.content_length;
    if (g_src.size <= 0 || g_src.size > MVSRC_MAX_SIZE) {
        mlog("mvsrc: bad content-length %lld", g_src.size);
        mvsrc_release();
        return -1;
    }

    g_src.wfd = sceIoOpen(g_src.part, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (g_src.wfd < 0) {
        mlog("mvsrc: cannot create %s", g_src.part);
        mvsrc_release();
        return -1;
    }
    if (mvsrc_open_rfd() < 0) {
        mlog("mvsrc: cannot open %s for read", g_src.part);
        mvsrc_release();
        return -1;
    }

    g_src.rlock = sceKernelCreateSema("mvsrc_rd", 0, 1, 1, NULL);
    if (g_src.rlock < 0) {
        mlog("mvsrc: sema failed 0x%08X", g_src.rlock);
        mvsrc_release();
        return -1;
    }

    g_source.opaque = &g_src;
    g_source.size = g_src.size;
    g_source.read_at = mvsrc_read_at;
    g_source.cancel = mvsrc_do_cancel;
    g_source.first_frame = NULL;
    g_source.interrupt_reads = NULL;      /* P0：不做 seek 重启 */

    g_open = 1;
    g_src.thid = sceKernelCreateThread("mvsrc_dl", mvsrc_thread, MVSRC_PRIO,
                                       MVSRC_STACK, PSP_THREAD_ATTR_USER, NULL);
    if (g_src.thid < 0) {
        mlog("mvsrc: thread create failed 0x%08X", g_src.thid);
        g_open = 0;
        mvsrc_release();
        g_src.thid = -1;
        return -1;
    }
    if (sceKernelStartThread(g_src.thid, 0, NULL) < 0) {
        mlog("mvsrc: thread start failed");
        sceKernelDeleteThread(g_src.thid);
        g_src.thid = -1;
        g_open = 0;
        mvsrc_release();
        return -1;
    }

    mlog("mvsrc: open %lld bytes -> %s", g_src.size, g_src.part);
    return 0;
}

void mvsrc_close(void)
{
    if (!g_inited) return;      /* 从未 open：g_src 里的 UID 还是静态 0，不能当真的 fd 用 */

    if (g_src.thid >= 0) {
        SceUInt tmo = MVSRC_CLOSE_MS * 1000;

        g_src.cancel = 1;
        if (sceKernelWaitThreadEnd(g_src.thid, &tmo) < 0) {
            /* 线程还卡在驱动里：绝不能关它正在用的 fd/http，宁可泄漏 */
            mlog("mvsrc: thread stuck, keep fds (leak)");
            g_src.thid = -1;
            g_open = 0;
            memset(&g_source, 0, sizeof(g_source));
            return;
        }
        sceKernelDeleteThread(g_src.thid);
        g_src.thid = -1;
    }
    mvsrc_release();
    memset(&g_source, 0, sizeof(g_source));
    g_open = 0;
}

void mvsrc_abort(void)
{
    g_src.cancel = 1;
}

int mvsrc_started(void)
{
    return g_open && !g_src.done && !g_src.err && !g_src.cancel;
}

long long mvsrc_size(void)
{
    return g_open ? g_src.size : -1;
}

long long mvsrc_available(void)
{
    return g_open ? (long long)g_src.avail : 0;
}

int mvsrc_complete(void)
{
    return g_open && g_src.done;
}

int mvsrc_error(void)
{
    return g_open && g_src.err;
}

const PspVideoSource *mvsrc_source(void)
{
    return g_open ? &g_source : NULL;
}

const char *mvsrc_part_path(void)
{
    return g_open ? g_src.part : NULL;
}
