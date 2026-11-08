#include <pspkernel.h>
#include <pspiofilemgr.h>

#include "psp_video.h"

PSP_MODULE_INFO("MELODYMV", 0, 1, 1);
#ifndef MV_HEAP_KB
#define MV_HEAP_KB (8 * 1024)
#endif
PSP_HEAP_SIZE_KB(MV_HEAP_KB);

/* 宿主 API 版本号（loader.c 里有一份同样的定义）。老 loader/老 PRX 不匹配时
 * 靠 magic 判断，绝不能盲调第 4 个函数指针。 */
#define MV_API_MAGIC 0x4D563537u   /* 'MV57': explicit unsafe-stop result contract */

typedef struct {
    int (*play)(const PspVideoOptions *opt, PspVideoResult *res);
    void (*stop)(void);
    unsigned magic;
    void (*release_heap)(void);
} MvApi;

/* ------------------------------------------------------------------ */
/* 卸载前归还 8MB 分区堆                                                */
/* ------------------------------------------------------------------ */
/* PSP_HEAP_SIZE_KB 只给大小。真正的 8MB 是 libcglue 的 _sbrk 在第一次 malloc
 * 时用 sceKernelAllocPartitionMemory(USER, "heap", 8MB) 一次性划走的用户分区，
 * UID 存在 libcglue 的 __psp_heap_blockid 里。模块自己从不释放它，所以
 * sceKernelUnloadModule 只还回代码镜像（约 4.7MB），8MB 永远漏在外面 ——
 * 真机上第一次 MV 播完 free 从 14894K 掉到 1979K、第二次起播就重启，就是这个。
 *
 * 注意：这个 PRX 的 module_stop **不会被调用**（.lib.ent 入口表是空的，
 * 实测 mvprx.txt 里从来没有 stop 那两行），所以在 module_stop 里释放没用，
 * 必须由宿主在卸载前显式调 release_heap()（loader.c 的 do_mv_unload）。
 * __psp_free_heap() 是 libcglue 里那个
 *     if (__psp_heap_blockid > 0) sceKernelFreePartitionMemory(__psp_heap_blockid);
 *
 * 这里故意不调 __libcglue_init()：这版 SDK 的它把第二个参数($a1)当 cwd 指针
 * 做 __init_cwd(*(void**)a1) → strncpy 最多 1024 字节；PRX 的 argp 是 loader
 * 线程栈上的指针，真机上读到栈外就是重启，风险大于收益。malloc/_sbrk 本来
 * 就不需要它（块是懒分配的，今天已经在用）。 */
extern int __psp_heap_blockid;
extern void __psp_free_heap(void);

/* ------------------------------------------------------------------ */
/* newlib 递归锁的初始化（真机冻结的根因修复）                          */
/* ------------------------------------------------------------------ */
/* 这个 PRX 故意不调 __libcglue_init()（它把 argp 当 cwd 指针，真机会   */
/* 读到栈外）。但 libcglue 的初始化还负责在 .bss 的 __lock___* 对象上    */
/* 调 sceKernelCreateLwMutex —— 不调它，__malloc_lock() 每次都拿着全 0   */
/* 的 workarea 调 sceKernelLockLwMutex，内核返回错误，malloc 实际上没有  */
/* 互斥。pspvideo 的视频解码线程和 AAC 解码线程同时 malloc/free 时会踩  */
/* 坏堆：表现就是"只有 MV 播一段时间后整机冻结"（去掉 MV 音频后不再复   */
/* 现），且与 catch-up/flush 的分配抖动正相关。                         */
/* 这里在 module_start 里、任何线程启动之前，就地创建这几个递归锁。     */
extern char __lock___malloc_recursive_mutex[];
extern char __lock___sfp_recursive_mutex[];
extern char __lock___env_recursive_mutex[];
extern char __lock___tz_mutex[];
extern char __lock___dd_hash_mutex[];

static void mv_log(const char *tag, unsigned hex, const char *mid, unsigned dec);

static int newlib_lock_init(char *obj, const char *name)
{
    return sceKernelCreateLwMutex((SceLwMutexWorkarea *)obj, name,
                                  PSP_LW_MUTEX_ATTR_RECURSIVE, 0, NULL);
}

static void newlib_locks_init(void)
{
    unsigned bad = 0;

    if (newlib_lock_init(__lock___malloc_recursive_mutex, "mv_malloc") < 0) ++bad;
    if (newlib_lock_init(__lock___sfp_recursive_mutex, "mv_sfp") < 0) ++bad;
    if (newlib_lock_init(__lock___env_recursive_mutex, "mv_env") < 0) ++bad;
    if (newlib_lock_init(__lock___tz_mutex, "mv_tz") < 0) ++bad;
    if (newlib_lock_init(__lock___dd_hash_mutex, "mv_dd") < 0) ++bad;
    mv_log("mvprx: newlib locks fail=", bad, " ", 5);
}

/* 释放后不能再走 libc 的 printf/malloc，所以自带一个只用 sceIo 的小日志器。 */
static int put_udec(char *d, unsigned v)
{
    char t[12];
    int n = 0;
    int i;

    if (!v) { d[0] = '0'; return 1; }
    while (v && n < 10) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (i = 0; i < n; ++i) d[i] = t[n - 1 - i];
    return n;
}

static int put_hex8(char *d, unsigned v)
{
    static const char h[] = "0123456789ABCDEF";
    int i;

    for (i = 7; i >= 0; --i) { d[i] = h[v & 15u]; v >>= 4; }
    return 8;
}

/* tag + %08X + mid + %u + "\n" */
static void mv_log(const char *tag, unsigned hex, const char *mid, unsigned dec)
{
    char buf[128];
    int n = 0;
    const char *p;

    for (p = tag; *p && n < 96; ++p) buf[n++] = *p;
    n += put_hex8(buf + n, hex);
    for (p = mid; *p && n < 112; ++p) buf[n++] = *p;
    n += put_udec(buf + n, dec);
    buf[n++] = '\n';
    {
        int fd = sceIoOpen("./mvprx.txt",
                           PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);

        if (fd >= 0) {
            sceIoWrite(fd, buf, n);
            sceIoClose(fd);
            sceIoSync("ms0:", 0);
        }
    }
}

/* 由 loader 在 sceKernelStopModule 之前调用：把 8MB sbrk 堆还给系统。
 * 清 0 是为了重复调用时 __psp_free_heap 什么也不做（防重复释放）。 */
static void release_heap(void)
{
    int uid = __psp_heap_blockid;

    mv_log("mvprx: release heapid=", (unsigned)uid, " free=",
           (unsigned)(sceKernelTotalFreeMemSize() / 1024));
    if (uid > 0) {
        __psp_free_heap();
        __psp_heap_blockid = 0;
    }
    mv_log("mvprx: released heapid=", (unsigned)__psp_heap_blockid, " free=",
           (unsigned)(sceKernelTotalFreeMemSize() / 1024));
}

static MvApi g_api = { psp_video_play, psp_video_stop, MV_API_MAGIC, release_heap };

__attribute__((used, noinline)) static int mv_entry_pad(int x)
{
    return x + 1;
}

/* no libcglue init in MV PRX: pspvideo uses its own heap path (see above) */

int module_start(SceSize argc, void *argp)
{
    MvApi **pp = (MvApi **)(*(void ***)argp);

    (void)mv_entry_pad;
    newlib_locks_init();
    if (argc >= (SceSize)sizeof(MvApi *) && pp) *pp = &g_api;
    /* 此刻还没 malloc，heapid 应为 0；真机上用来确认是新装的那份模块 */
    mv_log("mvprx: start heapid=", (unsigned)__psp_heap_blockid, " free=",
           (unsigned)(sceKernelTotalFreeMemSize() / 1024));
    return 0;
}

/* 这个 PRX 的 module_stop 实际上不会被内核调用（见上面的说明），保留成
 * 幂等的兜底：万一某个固件调了，也能把 8MB 还回去。 */
int module_stop(SceSize argc, void *argp)
{
    (void)argc;
    (void)argp;
    release_heap();
    return 0;
}

void _exit(int code)
{
    (void)code;
    for (;;) { }
}
