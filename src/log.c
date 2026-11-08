#include "log.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH "./log.txt"
#define LOG_FLUSH_EVERY 8      /* 每 8 行刷一次，兼顾崩溃可查与记忆棒寿命 */
/* 调试期可以打开：强制把记忆棒缓存写进卡，重启时不丢最后几行 */
#define LOG_SYNC 0

static char g_buf[16384];
static FILE *g_log;
static int g_inited;
static SceInt64 g_t0;
static int g_io_lock = -1;
static int g_pending_lines;

/* ------------------------------------------------------------------ */
/* 全局 stdio 锁                                                       */
/* ------------------------------------------------------------------ */

/* PSP 的信号量没有 owner：线程被 sceKernelTerminateDeleteThread 强杀时，
 * 如果它正握着这把锁，锁就永远回不来 —— 之后任何线程的第一次 io_lock()
 * （哪怕只是 mlog 一行）都会永久冻结主线程。PPSSPP 上「菜单停在播放 MV、
 * 界面不动、不崩」就是这么来的：mv_begin 里 api_shutdown() 3 秒等不到
 * api 线程就强杀，而它可能正握着 io_lock / api 的 g_lock。
 *
 * 所以这里把「无限等」改成「最多等 100ms」：
 *   - 拿不到就跳过这次写盘（日志丢一行，比整机卡死好）；
 *   - io_unlock() 会认自己有没有真拿到，没拿到就绝不放别人的锁。
 * 正常路径（锁没被毒死）行为和以前完全一样。 */
#define IO_LOCK_TIMEOUT_US  (100 * 1000)
#define IO_HOLD_MAX         4

/* 谁现在拿着锁（线程 id）。只有持有者能 unlock —— 超时没拿到的线程
 * 绝不能 signal，否则会把别人的锁放掉；同时也不会留下「脏标记」把
 * 自己下一次成功拿到的锁又锁死（第一版就踩过这个坑）。 */
static SceUID g_hold_thid[IO_HOLD_MAX] = { -1, -1, -1, -1 };
static volatile int g_io_timeouts;      /* 超时次数（诊断用） */

/* 修复：信号量改成在 log_open() 里先建好（那时别的线程还没起来），
 * 避免第一次 io_lock() 时几个线程同时 sceKernelCreateSema 抢着创建。 */
static void log_io_init(void)
{
    if (g_io_lock < 0)
        g_io_lock = sceKernelCreateSema("melody_io", 0, 1, 1, NULL);
}

static int hold_add(SceUID me)
{
    int i;

    for (i = 0; i < IO_HOLD_MAX; ++i) {
        if (g_hold_thid[i] == me) return 1;
        if (g_hold_thid[i] < 0) { g_hold_thid[i] = me; return 1; }
    }
    return 0;
}

static int hold_del(SceUID me)
{
    int i;

    for (i = 0; i < IO_HOLD_MAX; ++i) {
        if (g_hold_thid[i] == me) { g_hold_thid[i] = -1; return 1; }
    }
    return 0;
}

/* 1 = 拿到了锁；0 = 超时（调用方要跳过这次写盘） */
static int io_trylock(void)
{
    SceUInt tmo = IO_LOCK_TIMEOUT_US;

    /* 修复：正常路径由 log_open() 提前创建；万一有更早的调用，保留懒创建兜底 */
    if (g_io_lock < 0) log_io_init();
    if (g_io_lock < 0) return 0;
    if (sceKernelWaitSema(g_io_lock, 1, &tmo) < 0) {
        ++g_io_timeouts;
        return 0;
    }
    hold_add(sceKernelGetThreadId());
    return 1;
}

/* PSP 的 newlib stdio 不是线程安全的：四个线程同时 fopen/fwrite 会踩坏
 * FILE 链表，表现为随机假死。所有文件操作都走这把锁（别嵌套调用）。
 * 注意：超时返回时调用方仍然会往下走（旧调用点都不检查返回值），
 * 这是有意的取舍 —— 锁被毒死时宁可偶尔无锁写一次，也不要整机卡死。 */
void io_lock(void)
{
    (void)io_trylock();
}

void io_unlock(void)
{
    SceUID me = sceKernelGetThreadId();

    if (g_io_lock < 0) return;
    if (!hold_del(me)) return;            /* 本线程没持有：绝不放别人的锁 */
    sceKernelSignalSema(g_io_lock, 1);
}

/* 强制杀过线程之后调用：旧锁可能永远回不来，直接换一把新的。
 * 还在等旧锁的线程会拿到一个错误码（不会卡死），下一次 io_lock() 用新锁。 */
void io_lock_reset(void)
{
    int old = g_io_lock;
    int i;

    for (i = 0; i < IO_HOLD_MAX; ++i) g_hold_thid[i] = -1;
    g_io_lock = sceKernelCreateSema("melody_io2", 0, 1, 1, NULL);
    if (old >= 0) sceKernelDeleteSema(old);
}

int io_lock_timeouts(void) { return g_io_timeouts; }

/* ------------------------------------------------------------------ */
/* 日志                                                                */
/* ------------------------------------------------------------------ */

void log_open(void)
{
    if (g_inited) return;
    g_inited = 1;
    g_t0 = sceKernelGetSystemTimeWide();

    log_io_init();
    g_pending_lines = 0;

    if (!io_trylock()) return;        /* 锁被毒死：这次开不了日志，别死等 */
    sceIoMkdir("ms0:/PSP/GAME", 0777);
    sceIoMkdir("ms0:/PSP/GAME/MELODY", 0777);
    /* 轮转：上一次的日志保留成 log.prev.txt。否则 fopen("w") 会把崩溃现场
     * 直接截断——用户重启后再点"上传日志"，传上去的只有空白的新日志。 */
    sceIoRemove("./log.prev.txt");
    sceIoRename(LOG_PATH, "./log.prev.txt");
    g_log = fopen(LOG_PATH, "w");
    /* 整块缓冲：播放时每条日志都刷记忆棒会锁住 FAT，音频线程读 mp3 就被卡住 */
    if (g_log) setvbuf(g_log, g_buf, _IOFBF, sizeof(g_buf));
    io_unlock();
}

void log_flush(void)
{
    if (!io_trylock()) return;        /* 拿不到锁就等下一次刷，别死等 */
    if (g_log) {
        fflush(g_log);
#ifdef LOG_SYNC
        sceIoSync("ms0:", 0);
#endif
        g_pending_lines = 0;
    }
    io_unlock();
}

void log_close(void)
{
    if (!io_trylock()) { g_log = NULL; return; }
    if (g_log) {
        fflush(g_log);
        fclose(g_log);
        g_log = NULL;
    }
    g_pending_lines = 0;
    io_unlock();
}

int log_ms(void)
{
    return (int)((sceKernelGetSystemTimeWide() - g_t0) / 1000);
}

/* 调试用：不经过 stdio / io_lock，直接系统调用写卡，
 * 用来确认「卡在 io_lock」还是「卡在 fopen」。默认可不用。 */
void log_raw(const char *s)
{
#if LOG_SYNC
    int fd = sceIoOpen("./raw.txt",
                       PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);

    if (fd >= 0) {
        sceIoWrite(fd, s, strlen(s));
        sceIoClose(fd);
    }
#else
    (void)s;
#endif
}

void mlog(const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    int ms;

    if (!g_inited) log_open();
    if (!g_log) return;

    ms = log_ms();

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (!io_trylock()) return;         /* 锁被强杀线程带走时丢这一行，不死等 */
    if (g_log) {
        fprintf(g_log, "[%6d] %s\n", ms, buf);
        /* 修复：不再每行都 fflush（每条日志刷一次记忆棒会卡 FAT），
         * 每 LOG_FLUSH_EVERY 行刷一次；log_flush()/log_close() 里强制刷。 */
        if (++g_pending_lines >= LOG_FLUSH_EVERY) {
            fflush(g_log);
            g_pending_lines = 0;
#ifdef LOG_SYNC
            sceIoSync("ms0:", 0);
#endif
        }
    }
    io_unlock();
}
