#include "thread_paths.h"
#include "mv_result_codes.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <pspdmac.h>
#include <pspdisplay.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "api.h"
#include "audio.h"
#include "gfx.h"
#include "log.h"
#include "melody_host.h"
#include "mv.h"
#include <unistd.h>
#include "skin_runtime.h"
#include "ui.h"
#include "update_gallery.h"
#include "store.h"
#include "mv_probe.h"
#include "net.h"
#include "sysinfo.h"
#include "psp_video.h"

#define MV_W 480
#define MV_H 270
#define MV_RGB565_BYTES (512 * 272 * 2)

static SceUID g_ready = -1;
static SceUID g_taken = -1;
static SceUID g_thread = -1;
static int g_faulted; /* Unconfirmed worker/ME stop: retain session until restart. */
static int g_audio_suspended;

static int mv_restore_audio(void)
{
    int r;
    if (!g_audio_suspended) return 0;
    if (g_faulted || g_thread >= 0) return -1;
    /* Consume exactly once, including resource-init failures. */
    g_audio_suspended=0;
    gfx_resume_renderer();
    r=audio_mv_resume();
    if (r < 0) mlog("mv: MP3 resume failed r=%d", r);
    return r;
}
static volatile int g_quit;
static volatile int g_frame_ready;
static volatile int g_finished;
static volatile int g_result;
static char g_path[256];
static char g_title[72];
static volatile int g_pos_ms;
static volatile int g_dur_ms;
static unsigned g_last_frame_us;    /* 看门狗：最后一次 DMA 成功的时间 */
/* 2026-11-01：看门狗只信主线程自己看到的计数，不再用跨线程时间戳。
 * 真机日志证明 g_last_frame_us 会在帧还在涨时被读成 6 秒旧值（跨模块回调
 * 线程写、主线程读，非 volatile）。 */
static int g_wd_last_frames;       /* 上次看到的已呈现帧数 */
static unsigned g_wd_frames_us;    /* 上次帧数变化的时间（主线程自己记） */
static unsigned g_start_us;         /* MV 启动时间（没有首帧也要能超时） */
static unsigned g_present_max_us;   /* present 最坏耗时（诊断 pacer 线程够不够） */
static int g_dma_result;            /* 最近一次 sceDmacMemcpy 返回值 */
static int g_got_frame;
static int g_frames;                /* 本次播放已经呈现的帧数（统计用） */
static unsigned g_frames_t0;
static unsigned g_present_wait_ms = 500;   /* present 等主线程取帧的总上限（mvpresent.txt 可调） */
static unsigned g_copy_us_max, g_post_us_max;
static unsigned long long g_copy_us_total, g_post_us_total;
static unsigned g_perf_frames;
static int g_present_timeouts;             /* 本次播放里超时丢帧的次数 */
static unsigned g_beat_us;                 /* mv: alive 心跳节流（2 秒一行） */
static unsigned g_pos_us;                  /* 播放位置最后一次前进的时间（整段进度看门狗） */
static int g_pos_last;                     /* 上一次看到的播放位置 */
/* pacer 线程写的暂停/seek 标志：暂停时当然没有新帧，看门狗要跳过，
 * 否则暂停 8 秒就会被当成卡死、强制退出 MV。 */
static volatile int g_mv_paused;
static volatile int g_mv_seeking;
static int s_api_released;   /* one-time: API buffers are freed before first MV */

/* ------------------------------------------------------------------ */
/* 起播内存闸门 + 诊断（真机第二次进 MV 会重启，见 docs/mv-second-start-crash.md）    */
/* ------------------------------------------------------------------ */
/* 一次 playback 的 PRX 成本 = 模块镜像 4.7MB + 它自己的 8MB sbrk 分区堆 = 12.9MB。
 * 那 8MB 是 PRX 里 _sbrk 第一次 malloc 时用 sceKernelAllocPartitionMemory
 * 划走的。以前模块常驻不卸载、这块也拿不回来，真机第一次播完只剩 ~2.0MB，
 * 第二次起播就重启。现在：mvprx.c 在 module_stop 里调 __psp_free_heap() 把 8MB
 * 还回系统，mv_join() 每次播完都卸载 → 每次起播都是「第一次」的余量（~14.9MB）。
 * 闸门保留作安全网：万一卸载没把内存还回来（或别的路径导致余量不足），
 * 直接在起播前失败（main.c 弹 toast），而不是让 PRX 在 2MB 里分配失败把机器带重启。
 *   mvminfree.txt = 阈值 KB（覆盖下面两个默认值；写 0 关闭闸门）
 *   mvkeep.txt    = 1 播完不卸载（A/B 对比旧行为用）
 *   mvforce.txt   = 1 强制放行（真机上故意复现崩溃时用）
 *   mvdiag.txt    = 1 每 250ms 记一行 mv: alive（定位崩在第几秒） */
#define MV_MIN_FREE_FIRST_KB   13664  /* software image + 7MiB heap + 256KiB thread */
#define MV_MIN_FREE_LOADED_KB  7456   /* 7MiB heap + thread + small guard */

static int g_sessions;       /* 本次启动里真正起播过的 MV 次数 */
static int g_block_reason;   /* 0=没被拒, 1=内存不足被拒（给 main.c 弹提示用） */
static int g_diag;           /* mvdiag.txt=1 */
static int g_mv_dma = 1;     /* mvdma.txt=0：跳过 dcache 写回 + DMA（只留 CPU 565 转换） */
/* mvosd.txt=0：关闭 MV 底部 OSD 与"底带保存/恢复"（二分 2026-11-01 闪退用）。
 * 默认开；怀疑 OSD 的 per-frame 底带 memcpy / NULL 路径局部 DMA 影响稳定性时关掉。 */
static int g_osd_enable = 1;
static unsigned g_diag_last; /* 诊断行节流 */
static int g_keep_loaded;    /* mvkeep.txt=1：播完不卸载（旧行为） */
static int g_prx_loaded;     /* PRX 现在是否常驻（决定闸门阈值） */

static int mv_memory_gate_blocks(int free_kb, int max_kb, int need_kb, int force)
{
    /* A split 9832K + 4100K has enough total, but loading a 6212K image
     * leaves neither hole large enough for the decoder's 7168K heap.
     * Keep the conservative contiguous reservation until layout is known. */
    return need_kb > 0 && (free_kb < need_kb || max_kb < need_kb) && !force;
}

/* Defined by this PRX's newlib glue. Query only our known allocation ID;
 * no kernel-only partition enumeration or reads through reported addresses. */
extern int __psp_heap_blockid;
static void mv_memory_diag(void)
{
    SceUID ids[64];
    int count = 0;
    int r;
    mlog("mv mem: root-heap id=%d base=%08X reserved=2048K",
         __psp_heap_blockid,
         (unsigned)sceKernelGetBlockHeadAddr(__psp_heap_blockid));
    r = sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids,
                                    (int)(sizeof(ids) / sizeof(ids[0])), &count);
    mlog("mv mem: thread-list r=%08X count=%d limit=64", (unsigned)r, count);
    if (r < 0) return;
    if (count > 64) count = 64;
    for (int i = 0; i < count; ++i) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(ids[i], &info) < 0) continue;
        /* Thread names are kernel allocation labels, never song/file data. */
        mlog("mv mem: thread id=%d name=%.31s stack=%08X bytes=%u attr=%08X",
             ids[i], info.name, (unsigned)info.stack,
             (unsigned)info.stackSize, (unsigned)info.attr);
    }
}

/* 读 ./<name> 里的整数；不存在/读不到就用 def。
 * 和 mvfast.txt / mvstream.txt 的约定一致：手写一个文件就能改行为，不用重编。 */
static int mv_cfg_int(const char *name, int def)
{
    char path[96];
    char b[16];
    int fd;
    int n;
    int v;

    snprintf(path, sizeof(path), APP_DIR "%s", name);
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return def;
    n = sceIoRead(fd, b, (int)sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) return def;
    b[n] = 0;
    {
        const char *p = b;

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (*p != '-' && (*p < '0' || *p > '9')) return def;   /* 乱写就按默认 */
    }
    v = atoi(b);
    return v;
}

/* 起播阶段的路标：绕过 stdio/io_lock、直接写 mvstep.txt。
 * mv_begin 是主线程上唯一会同时碰 api 线程、loader 握手、显存切换的地方，
 * 卡死时最后一行就是卡住的那一步（PPSSPP 菜单卡死就是这么定位的）。
 * mvdiag.txt=1 时每行再 sceIoSync，真机上才一定落卡。 */
static void mv_step(const char *tag)
{
    char b[96];
    int n = snprintf(b, sizeof(b), "#%d %-13s free=%dK max=%dK\n", g_sessions + 1, tag,
                     (int)(sceKernelTotalFreeMemSize() / 1024),
                     (int)(sceKernelMaxFreeMemSize() / 1024));
    int fd = sceIoOpen(APP_DIR "mvstep.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);

    if (fd < 0) return;
    sceIoWrite(fd, b, n);
    sceIoClose(fd);
    if (g_diag) sceIoSync("ms0:", 0);
}

static UiVideoPalette g_mv_palette;
static void mv_capture_palette(void){ui_video_palette(&g_mv_palette);}
static void mv_status(const char *msg, int pct)
{
    int w = SCR_W - 80;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_clear(g_mv_palette.panel);
    gfx_text_center(FS_LG, 0, SCR_W, 104, msg, g_mv_palette.text);
    gfx_fill_rect(40, 150, w, 6, g_mv_palette.track);
    gfx_fill_rect(40, 150, w * pct / 100, 6, g_mv_palette.accent);
    gfx_text_center(FS_SM, 0, SCR_W, 166, "MELODY MV", g_mv_palette.muted);
    gfx_flip();
}

/* OSD 直接画在 8888 的 MV 缓冲上；下一帧 DMA 会整块盖掉，所以每帧都重画。
 * 只有顶部 20 行 + 底部 14 行，约 1.6 万像素，非缓存写 ~0.5ms。 */
/* ---- MV 内嵌 OSD（565）----
 * 直接画在 g_mv_ram 的底部 30 行：进度条 + "mm:ss / mm:ss"。
 * 注意：不能调 gfx_text/gfx_fill_rect —— 那些画到 UI 后缓冲，而且这个回调
 * 跑在 pts_present 线程（16KB 栈），只能手写 5x7 点阵。
 * pixels==NULL 时（暂停/seek 预览/缓冲）用上一帧保存的干净底带恢复再画，
 * 否则进度条会一帧帧叠在一起糊成一块。 */
#define MV_OSD_ROWS 30
#define MV_OSD_Y0   (MV_H - MV_OSD_ROWS + 1)   /* 241 */
static int g_use_source;
static unsigned short g_osd_band[MV_OSD_ROWS][512];
static int g_osd_band_ok;

static const unsigned char g_font5x7[17][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E},   /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00},   /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46},   /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31},   /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10},   /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39},   /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30},   /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03},   /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36},   /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E},   /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00},   /* : */
    {0x20, 0x10, 0x08, 0x04, 0x02},   /* / */
    {0x00, 0x41, 0x7F, 0x41, 0x00},   /* I */
    {0x63, 0x13, 0x08, 0x64, 0x63},   /* % */
    {0x7f,0x08,0x14,0x22,0x41}, /* K */
    {0x7f,0x49,0x49,0x49,0x36}, /* B */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
};

/* 565 颜色（和 gfx_rgb 一样是 BGR：红在低 5 位） */
#define OSD_RED    (g_mv_palette.accent)
#define OSD_TRACK  (g_mv_palette.track)
#define OSD_WHITE  0xFFFF
#define OSD_YELLOW (g_mv_palette.accent)

static void osd_glyph(unsigned short *dst, int x, int y, int gi, int sc, unsigned short col)
{
    const unsigned char *g = g_font5x7[gi];

    for (int c = 0; c < 5; ++c) {
        unsigned char bits = g[c];

        for (int r = 0; r < 7; ++r) {
            if (!(bits & (1u << r))) continue;
            for (int dy = 0; dy < sc; ++dy) {
                unsigned short *p = dst + (y + r * sc + dy) * 512 + x + c * sc;

                for (int dx = 0; dx < sc; ++dx) p[dx] = col;
            }
        }
    }
}

static void osd_text(unsigned short *dst, int x, int y, const char *s, int sc, unsigned short col)
{
    for (; *s; ++s) {
        int gi = -1;

        if (*s >= '0' && *s <= '9') gi = *s - '0';
        else if (*s == ':') gi = 10;
        else if (*s == '/') gi = 11;
        else if (*s == 'I') gi = 12;
        else if (*s == '%') gi = 13;
        else if (*s == 'K') gi = 14;
        else if (*s == 'B') gi = 15;
        else if (*s == 's') gi = 16;
        if (gi >= 0) osd_glyph(dst, x, y, gi, sc, col);
        x += 6 * sc;
    }
}

static int osd_fmt_time(char *out, int ms)
{
    int s = ms / 1000;

    if (s < 0) s = 0;
    return snprintf(out, 16, "%02d:%02d", s / 60, s % 60);
}

static void mv_osd_draw(unsigned short *ram, const PspVideoStatus *st)
{
    int pos = (int)st->position_ms;
    int dur = (int)st->duration_ms;
    char a[16], b[16], line[40];
    int len, x;

    if (pos < 0) pos = 0;
    if (dur < 0) dur = 0;
    if (dur > 0 && pos > dur) pos = dur;

    /* 底部压暗 50%，白字才看得清（纯整数混合） */
    for (int y = MV_OSD_Y0 + 4; y <= MV_H; ++y) {
        unsigned short *row = ram + y * 512;

        for (int i = 0; i < 512; ++i) row[i] = (unsigned short)((row[i] >> 1) & 0x7BEF);
    }

    /* 进度条：x=16..495，y=MV_OSD_Y0+7..+11 */
    {
        int bx = 16, by = MV_OSD_Y0 + 7, bw = 480, bh = 5;

        for (int y = by; y < by + bh; ++y) {
            unsigned short *row = ram + y * 512;

            for (int i = bx; i < bx + bw; ++i) row[i] = OSD_TRACK;
        }
        if(g_use_source&&!mv_stream_cancelled()){
            long long total=mv_stream_total(),available=mv_stream_available();
            int buffered=total>0?(int)(available*bw/total):0;if(buffered>bw)buffered=bw;
            for(int y=by;y<by+bh;++y)for(int i=bx;i<bx+buffered;++i)ram[y*512+i]=0x7bef;
        }
        if (dur > 0 && pos > 0) {
            int fill = (int)((long long)pos * bw / dur);

            if (fill > bw) fill = bw;
            for (int y = by; y < by + bh; ++y) {
                unsigned short *row = ram + y * 512;

                for (int i = bx; i < bx + fill; ++i) row[i] = OSD_RED;
            }
        }
        if (st->seeking && dur > 0) {
            int px = bx + (int)((long long)st->preview_ms * bw / dur);

            if (px < bx) px = bx;
            if (px > bx + bw - 2) px = bx + bw - 2;
            for (int y = by - 2; y < by + bh + 2; ++y) {
                unsigned short *row = ram + y * 512;

                row[px] = OSD_WHITE;
                row[px + 1] = OSD_WHITE;
            }
        }
    }

    /* 时间：scale 2（10x14），居中 */
    osd_fmt_time(a, pos);
    osd_fmt_time(b, dur);
    len = snprintf(line, sizeof(line), "%s / %s", a, b);
    x = (512 - len * 12) / 2;
    if (x < 16) x = 16;
    osd_text(ram, x, MV_OSD_Y0 + 16, line, 2, OSD_WHITE);

    if(g_use_source){long long total=mv_stream_total();if(total>0){char percent[16];int pct=(int)(mv_stream_available()*100/total);if(pct<0)pct=0;if(pct>100)pct=100;snprintf(percent,sizeof(percent),"%d%%",pct);osd_text(ram,440,MV_OSD_Y0+19,percent,1,st->buffering?OSD_YELLOW:0xbdf7);}}
    if(st->buffering){
        unsigned phase=(sceKernelGetSystemTimeLow()/180000u)%4;
        for(unsigned dot=0;dot<4;++dot)for(int y=MV_OSD_Y0+19;y<MV_OSD_Y0+23;++y)for(int x=20+(int)dot*9;x<24+(int)dot*9;++x)ram[y*512+x]=dot==phase?OSD_YELLOW:OSD_TRACK;
        for(int y=MV_OSD_Y0+7;y<MV_OSD_Y0+12;++y)for(int i=16;i<24;++i)ram[y*512+i]=OSD_YELLOW;
    }
    if(store_show_speed()){char rate[20];snprintf(rate,sizeof(rate),"%d KB/s",net_speed_kbps());
     osd_text(ram,430,6,rate,1,OSD_WHITE);
     /* The compact video HUD uses the same KB/s rate as ordinary pages. */
}
    if (st->paused) osd_text(ram, 20, MV_OSD_Y0 + 16, "II", 2, OSD_YELLOW);
}

/* 解码线程的呈现回调：RGBA -> RAM 里的 565 -> 一次 DMA 到显存。
 * App沿用现有565显示；独立原生AVC已验证8888，因此不把本路径的切换限制当硬件格式限制。 */
static int mv_present(void *ud, const uint32_t *pixels, const PspVideoStatus *status)
{
    unsigned short *ram = gfx_mv_ram();
    void *vram = gfx_mv_vram();
    int osd = 0;
    unsigned copy_begin = 0, copy_end = 0;

    (void)ud;
    if (status) {
        g_pos_ms = (int)status->position_ms;
        g_dur_ms = (int)status->duration_ms;
        g_mv_paused = status->paused ? 1 : 0;
        g_mv_seeking = (status->seeking || status->buffering) ? 1 : 0;
        /* Diagnostics exclude intentional pause/rebuffer gaps from frame cadence. */
        static unsigned report_us,frame_us,frames,max_gap;
        unsigned now=sceKernelGetSystemTimeLow();
        if(status->paused||status->buffering||status->seeking){frame_us=0;max_gap=0;}
        else if(pixels){if(frame_us&&now-frame_us>max_gap)max_gap=now-frame_us;frame_us=now;frames++;}
        if(now-report_us>=1000000u){mlog("mv playback: position=%d real_frames=%u frame_gap=%ums paused=%d buffering=%d",g_pos_ms,frames,max_gap/1000,status->paused,status->buffering);report_us=now;frames=0;max_gap=0;}
        /* 用户要求 MV 页常显进度模块：不再依赖 PRX 的 3 秒自动隐藏 */
        osd = 1;
        (void)0;
    }
    if (g_quit || !ram || !vram) return 0;
    /* pixels==NULL：PRX 要求"重画最后一帧"（暂停 / seek 预览 / 缓冲）。
     * 还没存过底带就直接放弃，避免画出半帧。 */
    if (!pixels && (!g_osd_enable || !g_osd_band_ok)) return 0;

    /* ---- 写帧许可：单缓冲 scanout 上唯一的互斥 ----
     * 下面那次 DMA 的目标 gfx_mv_vram() 就是显示控制器正在扫描的显存，
     * g_mv_ram 又是解码线程唯一的重写目标。主线程还没取走上一帧就继续写，
     * 等于 CPU/DMA 和显示控制器同时碰同一块内存 —— 真机 PSP-1000 卡死的
     * 那条路径（见 docs/nc0-corruption.md §7）。
     * 以前是 DMA 之后等 200ms 就往下走，超时照样覆盖。现在把等待挪到写之前：
     * 2ms 轮询，主线程取走上一帧（g_frame_ready 归零）才允许写这一帧；
     * g_quit 立刻放弃；总截止 g_present_wait_ms（默认 500ms，mvpresent.txt
     * 可调），超时就丢掉这一帧 —— 不写 ram、不做 DMA，也就没有竞态。
     * 丢帧不是死状态：主线程回来后 g_frame_ready 归零，下一帧立刻恢复。 */
    {
        unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();
        unsigned limit = g_present_wait_ms * 1000u;

        while (g_frame_ready && !g_quit) {
            if ((unsigned)(sceKernelGetSystemTimeLow() - t0) >= limit) {
                ++g_present_timeouts;
                /* 只记第一次 + 每 30 次：25fps 下不会刷屏，但能看出「开始丢」 */
                if (g_present_timeouts == 1 || (g_present_timeouts % 30) == 0) {
                    mlog("mv: present timeout %dms f=%d n=%d",
                         (int)((sceKernelGetSystemTimeLow() - t0) / 1000), g_frames,
                         g_present_timeouts);
                }
                return 0;
            }
            sceKernelDelayThread(2000);
        }
        if (g_quit) return 0;
    }

    if (pixels) {
    /* 跳过第 0 行哨兵（512px），可见区 480x270 从 y=1 开始；第 0 行保持黑 */
    copy_begin = sceKernelGetSystemTimeLow();
    /* MV56 contract: row stride remains 2048 bytes; first 1024 are BGR565.
     * Loader rejects legacy RGBA PRXs before any presenter call. */
    for (int y = 1; y <= MV_H; ++y)
        memcpy(ram + y * 512, pixels + y * 512, 512 * sizeof(unsigned short));
    copy_end = sceKernelGetSystemTimeLow();

    /* 保存 OSD 区域的干净底带（必须在画 OSD 之前），供暂停/seek 预览时恢复 */
    if (g_osd_enable) for (int i = 0; i < MV_OSD_ROWS; ++i)
        memcpy(g_osd_band[i], ram + (MV_OSD_Y0 + i) * 512,
               512 * sizeof(unsigned short));
    if (g_osd_enable) g_osd_band_ok = 1;
    } else {
        /* pixels==NULL：恢复上一帧的干净底带，再叠 OSD */
        if (g_osd_enable) for (int i = 0; i < MV_OSD_ROWS; ++i)
            memcpy(ram + (MV_OSD_Y0 + i) * 512, g_osd_band[i],
                   512 * sizeof(unsigned short));
    }

    if (g_osd_enable && osd && status) mv_osd_draw(ram, status);

    /* DMA 不 snoop D-cache：先把 RAM 帧写回内存，否则会搬旧数据。
     * mvdma.txt=0 时整段跳过（保留 565 转换和节拍，只去掉写回+DMA），
     * 用来判断冻结是否发生在“DMA / 扫描输出”这条路径上：画面会停在
     * 第一帧（黑屏），但解码、音频、看门狗、日志全部照常。 */
    if (g_mv_dma) {
        if (pixels) {
            sceKernelDcacheWritebackRange(ram, MV_RGB565_BYTES);
            {
                unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();
                int dr = sceDmacMemcpy(vram, ram, MV_RGB565_BYTES);
                unsigned dt = (unsigned)sceKernelGetSystemTimeLow() - t0;

                if (dt > g_present_max_us) g_present_max_us = dt;
                g_dma_result = dr;
            }
        } else {
            /* 暂停/seek 预览：只有底部 OSD 带变了，别每 33ms 重写整个 278KB
             * 帧（白白占 8MB/s 总线，还可能干扰音频 DMA）。 */
            unsigned char *src = (unsigned char *)ram + MV_OSD_Y0 * 512 * 2;
            unsigned char *dst = (unsigned char *)vram + MV_OSD_Y0 * 512 * 2;
            unsigned int len = MV_OSD_ROWS * 512 * 2;

            sceKernelDcacheWritebackRange(src, len);
            g_dma_result = sceDmacMemcpy(dst, src, len);
        }
    }

    if (pixels) {
    g_last_frame_us = (unsigned)sceKernelGetSystemTimeLow();
    {
        unsigned copy_us = copy_end - copy_begin;
        unsigned post_us = g_last_frame_us - copy_end;
        g_copy_us_total += copy_us;
        g_post_us_total += post_us;
        if (copy_us > g_copy_us_max) g_copy_us_max = copy_us;
        if (post_us > g_post_us_max) g_post_us_max = post_us;
        ++g_perf_frames;
    }
    if (!g_got_frame) {
        mlog("mv: first frame at %ums (vram=%08X src=%08X dma=%08X pmax=%uus)",
             (g_start_us ? (g_last_frame_us - g_start_us) / 1000 : 0),
             (unsigned)vram, (unsigned)pixels, g_dma_result, g_present_max_us);
    }
    g_got_frame = 1;
    if (!g_frames) g_frames_t0 = g_last_frame_us;
    if (++g_frames % 30 == 0) {
        unsigned dt = g_last_frame_us - g_frames_t0;
        mlog("mv565 perf frames=%u copy_avg=%uus copy_max=%uus osd_cache_dma_avg=%uus post_max=%uus",
             g_perf_frames, (unsigned)(g_copy_us_total / g_perf_frames), g_copy_us_max,
             (unsigned)(g_post_us_total / g_perf_frames), g_post_us_max);

        mlog("mv: %d frames in %ums (%.1f fps, pos=%dms, free=%dK max=%dK vc=%u stk=%d dma=%08X pmax=%uus)",
             g_frames, dt / 1000, dt ? g_frames * 1000000.0f / (float)dt : 0.0f,
             g_pos_ms, sysi_free_kb(), sysi_maxblock_kb(), sceDisplayGetVcount(),
             sceKernelGetThreadStackFreeSize(sceKernelGetThreadId()), g_dma_result,
             g_present_max_us);
    }
    }
    g_frame_ready = 1;
    /* 信号量已被 mv_join 删掉时（收尾竞态）不要再对着 -1 打信号：那会变成
     * 一张空转的死循环（PPSSPP 日志里见过 Sema(-1) 刷屏）。 */
    if (g_ready >= 0) sceKernelSignalSema(g_ready, 1);
    /* 交给主线程取帧的握手已在函数开头（写之前）完成：
     * 主线程 mv_display_tick() 里 g_frame_ready=0 + signal g_taken 就是许可。
     * 这里不能再等一次，否则「超时也照样往下写」的竞态会重新出现。 */
    return 0;
}

static const PspVideoPresenter g_presenter = { NULL, mv_present, NULL };

/* ------------------------------------------------------------------ */
/* 边下边播（实验，mvstream.txt=1 才启用）                              */
/* 下载线程顺序写 <final>.part；播放器用 PspVideoSource.read_at 读它，    */
/* 数据没到就阻塞等待（不依赖 FFmpeg 是否缓存文件大小）。                */
/* ------------------------------------------------------------------ */
static char g_src_url[1000];
static char g_src_part[512];
static char g_src_final[512];
static int g_src_mvid;
static char g_src_name[72],g_src_artist[64];
static volatile long long g_src_avail;
static volatile long long g_src_total;
static volatile int g_src_done;
static volatile int g_src_err;
static volatile int g_src_cancel;
static volatile int g_src_interrupted;
static volatile int g_src_renaming;
/* Reserved at startup: its stack must precede reclaimable high UI assets. */
static SceUID g_src_thid = -1;
static SceUID g_src_wake = -1;
static volatile int g_src_running;
static int g_use_source;
static int g_src_duration_ms;
static volatile int g_src_buffering;
static volatile int g_src_segmented,g_range_busy;
static volatile long long g_src_read_cursor;
/* Measured transfer rate over the last reporting window. The start gate and the
 * connecting page need it: a link slower than the video bitrate cannot sustain
 * streaming, and starting anyway is exactly the "MV always stutters" report. */
static volatile long long g_src_rate_bps;
static PspVideoSource g_source;
static unsigned char g_src_buf[16384];
/* A tail-moov MP4 must read its index before the sequential download reaches it. */
static SceUID g_range_lock=-1;
static Http g_range_http;
static unsigned char g_range_buf[64*1024];
static unsigned char g_range_previous[64*1024];
static long long g_range_previous_start;
static int g_range_previous_len;
static long long g_range_start;
static int g_range_len,g_range_disabled;
#include "mv_stream_contract.h"
static int g_src_fixture;
static uint64_t g_src_fixture_epoch;
static unsigned g_src_first_frame;
typedef struct{long long available,total;int done,error,cancel;}MvSourceSnapshot;
static MvSourceSnapshot src_snapshot(void){
 unsigned irq=sceKernelCpuSuspendIntr();
 MvSourceSnapshot v={g_src_avail,g_src_total,g_src_done,g_src_err,g_src_cancel||g_src_interrupted};
 int fixture=g_src_fixture;uint64_t epoch=g_src_fixture_epoch;
 sceKernelCpuResumeIntr(irq);
 if(fixture){v.available=mv_stream_fixture_available(v.total,(uint64_t)sceKernelGetSystemTimeWide()-epoch,256*1024,128*1024);v.done=v.available==v.total;}
 return v;
}
static void src_publish_available(long long value){unsigned irq=sceKernelCpuSuspendIntr();g_src_avail=value;sceKernelCpuResumeIntr(irq);}
static void src_publish_total(long long value){unsigned irq=sceKernelCpuSuspendIntr();g_src_total=value;sceKernelCpuResumeIntr(irq);}
static void src_first_frame(void*opaque){
 (void)opaque;MvSourceSnapshot v=src_snapshot();g_src_first_frame++;
 mlog("mv stream: FIRST FRAME avail=%lld total=%lld pending=%lld fixture=%d",v.available,v.total,v.total-v.available,g_src_fixture);
}

#define MV_STREAM_STACK  0x10000 /* HTTP/header/DNS call chain; never fit16KiB Http on stack. */
#define MV_STREAM_PRIO   0x16 /* Short receiving bursts preempt CPU-bound decode; audio/presenter stay higher. */

static void src_mark_done(void)
{
    /* 写 sidecar（和普通下载保持一致） */
    char meta[sizeof(g_src_final)];
    int tf;

    snprintf(meta, sizeof(meta), "%s", g_src_final);
    {
        char *dot = strrchr(meta, '.');

        if (dot) snprintf(dot, sizeof(meta) - (size_t)(dot - meta), ".txt");
    }
    tf = sceIoOpen(meta, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (tf >= 0) {
        char mb[220];
        int mn = snprintf(mb, sizeof(mb), "%s\n%s\n%d\n",g_src_name,g_src_artist,g_src_duration_ms);

        sceIoWrite(tf, mb, mn);
        sceIoClose(tf);
    }
    if(g_src_mvid>0)mv_index_add(g_src_mvid,g_src_total>0x7fffffff?0x7fffffff:(int)g_src_total);
}

static void src_download_finished(long long received,long long expected){
    if(!g_src_cancel&&!g_src_err&&(expected<=0||received!=expected))g_src_err=1;
}

static int src_open_range(Http*h,long long start,long long end,long long total,char*err,int cap){
 if(g_src_cancel||start<0||end<start||end>=total)return -1;
 if(http_open_range(h,g_src_url,start,end,6000,err,cap)<0)return -1;
 if(g_src_cancel||h->status!=206||h->range_start!=start||h->range_end!=end||h->range_total!=total||h->content_length!=end-start+1){http_close(h);return -1;}
 return 0;
}
/* Validate both the beginning and boundary before reusing an older partial file.
 * A changed/expired URL must not make us append unrelated bytes to that file. */
static int src_verify_partial(Http*h,int fd,long long size,long long total){
 unsigned char disk[4096];char err[80]={0};long long starts[2]={0,size>4096?size-4096:0};
 for(int probe=0;probe<2;++probe){
  long long start=starts[probe];int need=(int)(size-start);if(need>4096)need=4096;
  if(!need)return -1;
  if(sceIoLseek(fd,(SceOff)start,PSP_SEEK_SET)<0||sceIoRead(fd,disk,need)!=need)return -1;
  if(src_open_range(h,start,start+need-1,total,err,sizeof(err))<0)return -1;
  int have=0;unsigned began=sceKernelGetSystemTimeLow();
  while(have<need&&!g_src_cancel){int got=http_read(h,(char*)g_src_buf+have,need-have,500,&g_src_cancel,0,err,sizeof(err));if(got>0)have+=got;else if(got!=-1)break;if((unsigned)(sceKernelGetSystemTimeLow()-began)>=6000000u)break;}
  http_close(h);if(have!=need||memcmp(disk,g_src_buf,need))return -1;
 }
 return g_src_cancel?-1:0;
}
static int src_thread(SceSize args, void *argp)
{
#if defined(__PSP__)
    melody_bind_thread_directory();
#endif

    (void)args;
    (void)argp;
    /* One source worker at a time. Http embeds16KiB and exceeded the old
     *16KiB thread stack before the first network call. Keep it in BSS. */
    static Http h;
    char err[80] = "";
    int fd = -1;
    int r = 0,resume_attempts=0;
    long long done = 0;

    memset(&h,0,sizeof(h));
    mlog("mv stream: worker entered stack_free=%u",sceKernelCheckThreadStack());

    if (http_open_bulk(&h, g_src_url, 15000, err, sizeof(err)) < 0) {
        mlog("mv stream: open failed %s", err);
        g_src_err = 1;
        goto out;
    }
    unsigned transfer_begin=sceKernelGetSystemTimeLow();
    src_publish_total(h.content_length);
    if (h.status!=200||g_src_total <= 0) {
        http_close(&h);
        mlog("mv stream: no content-length");
        g_src_err = 1;
        goto out;
    }
    SceIoStat partial;
    if(sceIoGetstat(g_src_part,&partial)>=0&&partial.st_size>0){
        http_close(&h);
        if(partial.st_size>g_src_total){g_src_err=1;mlog("mv stream: partial larger than response; retained");goto out;}
        fd=sceIoOpen(g_src_part,PSP_O_RDWR,0777);
        if(fd<0||src_verify_partial(&h,fd,partial.st_size,g_src_total)<0){if(fd>=0)sceIoClose(fd);fd=-1;g_src_err=1;mlog("mv stream: partial validation failed; retained");goto out;}
        done=partial.st_size;
        if(sceIoLseek(fd,(SceOff)done,PSP_SEEK_SET)<0){sceIoClose(fd);fd=-1;g_src_err=1;goto out;}
        if(done<g_src_total&&src_open_range(&h,done,g_src_total-1,g_src_total,err,sizeof(err))<0){sceIoClose(fd);fd=-1;g_src_err=1;mlog("mv stream: resume range rejected; partial retained");goto out;}
        src_publish_available(done);mlog("mv stream: verified resume prefix=%lld total=%lld",done,g_src_total);
    }else fd = sceIoOpen(g_src_part, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        http_close(&h);
        mlog("mv stream: cannot write %s", g_src_part);
        g_src_err = 1;
        goto out;
    }
    src_mark_done(); /* Metadata records the task; filesystem distinguishes .part from final. */
    mlog("mv stream: %lld bytes -> %s", g_src_total, g_src_part);
    unsigned last_data_us=sceKernelGetSystemTimeLow(),last_report=last_data_us,max_wake_gap=0,write_us=0;
    long long report_done=0;unsigned wake=last_data_us;
    while (!g_src_cancel&&done<g_src_total) {
        unsigned tick=sceKernelGetSystemTimeLow();if(tick-wake>max_wake_gap)max_wake_gap=tick-wake;wake=tick;
        r = http_read(&h, (char *)g_src_buf, (int)sizeof(g_src_buf), 1000, &g_src_cancel, 0,
                      err, sizeof(err));
        if (r > 0) {
            unsigned write_start=sceKernelGetSystemTimeLow();
            if(r>g_src_total-done||sceIoWrite(fd, g_src_buf, r) != r) {
                mlog("mv stream: write failed");
                g_src_err = 1;
                break;
            }
            done += r;last_data_us=sceKernelGetSystemTimeLow();write_us+=last_data_us-write_start;
            src_publish_available(done);
            if(last_data_us-last_report>=2000000u){
                long long window=(long long)(last_data_us-last_report);
                long long gained=done-report_done;
                g_src_rate_bps=window>0?gained*1000000/window:0;
                mlog("mv transfer: rate=%dKiB/s loop_gap=%ums write=%ums paused=%d prefix=%lld",(int)((done-report_done)*1000000/(last_data_us-last_report)/1024),max_wake_gap/1000,write_us/1000,(int)g_mv_paused,done);
                last_report=last_data_us;report_done=done;max_wake_gap=0;write_us=0;
            }
            /* 让出 CPU：api 线程已经停了，解码/呈现要用 */
            sceKernelDelayThread(500);
        } else {
            if(g_src_cancel)break;
            if(r==-1&&!mv_stream_idle_expired(sceKernelGetSystemTimeLow(),last_data_us)){sceKernelDelayThread(20*1000);continue;}
            http_close(&h);int resumed=0;
            while(!g_src_cancel&&resume_attempts<2){
                ++resume_attempts;
                mlog("mv stream: recover attempt=%d prefix=%lld total=%lld read=%d",resume_attempts,done,g_src_total,r);
                if(src_open_range(&h,done,g_src_total-1,g_src_total,err,sizeof(err))==0){resumed=1;last_data_us=sceKernelGetSystemTimeLow();break;}
                http_close(&h);
            }
            if(!resumed){g_src_err=g_src_cancel?0:1;break;}
        }
    }
    http_close(&h);
    unsigned transfer_us=sceKernelGetSystemTimeLow()-transfer_begin;
    if(done>=256*1024&&transfer_us>1000000u){char rate[24];int count=snprintf(rate,sizeof(rate),"%d\n",(int)(done*1000000/transfer_us/1024));SceUID ratefd=sceIoOpen(APP_DIR "mvbandwidth.txt",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0600);if(ratefd>=0){sceIoWrite(ratefd,rate,count);sceIoClose(ratefd);}}
    int close_result=sceIoClose(fd);
    if(close_result<0)g_src_err=1;
    fd = -1;
    src_download_finished(done,g_src_total);
    /* Consumer EOF/exit can cancel after the final available publication.
     * Once every declared byte is written and the fd closed successfully,
     * retain the complete cache even when that playback was cancelled. */
    if (!g_src_err && done == g_src_total) {
        sceIoSync("ms0:", 0);
        g_src_renaming=1;
        sceIoRemove(g_src_final);
        if (sceIoRename(g_src_part, g_src_final) == 0) {
            g_src_done = 1; /* Publish final pathname immediately after rename. */
            src_mark_done();
            mlog("mv stream: done %lld", done);
        } else {
            g_src_err = 1;
            mlog("mv stream: rename failed");
        }
        g_src_renaming=0;
    } else {
        mlog("mv stream: stop done=%lld/%lld err=%d cancel=%d", done, g_src_total,
             g_src_err, g_src_cancel);
    }
out:
    return 0;
}

/* Never exit/delete between sessions: a late-created high stack can pin the
 * middle of the UI arena even after its images and GU pool have been freed. */
static int src_worker(SceSize args, void *argp)
{
#if defined(__PSP__)
    melody_bind_thread_directory();
#endif

    (void)args;
    (void)argp;
    for (;;) {
        if (sceKernelWaitSema(g_src_wake, 1, NULL) < 0) {
            sceKernelDelayThread(10000);
            continue;
        }
        src_thread(0, NULL);
        /* Publish retirement only after HTTP/file close and metadata work.
         * A new wake arriving before WaitSema is retained by the semaphore. */
        g_src_running = 0;
    }
    return 0;
}

int mv_stream_prepare_worker(void)
{
    if (g_src_thid >= 0) return 0;
    if (g_range_lock < 0)
        g_range_lock = sceKernelCreateSema("mv_range", 0, 1, 1, NULL);
    if (g_range_lock < 0) return g_range_lock;
    if (g_src_wake < 0)
        g_src_wake = sceKernelCreateSema("mv_wake", 0, 0, 1, NULL);
    if (g_src_wake < 0) return g_src_wake;
    g_src_thid = sceKernelCreateThread("mv_dl", src_worker, MV_STREAM_PRIO,
                                     MV_STREAM_STACK, PSP_THREAD_ATTR_USER, NULL);
    if (g_src_thid < 0) return g_src_thid;
    int result = sceKernelStartThread(g_src_thid, 0, NULL);
    if (result < 0) {
        /* Start failed: only this never-started thread may be deleted. */
        sceKernelDeleteThread(g_src_thid);
        g_src_thid = -1;
        return result;
    }
    mlog("mv stream: reserved idle worker uid=0x%08X stack=%u", g_src_thid,
         (unsigned)MV_STREAM_STACK);
    return 0;
}

static int src_queue_session(void)
{
    if (g_src_running || g_src_thid < 0) return -1;
    g_src_running = 1; /* Before wake: worker may preempt SignalSema. */
    int result = sceKernelSignalSema(g_src_wake, 1);
    if (result < 0) { g_src_running = 0; g_src_err = 1; return result; }
    return 0;
}

static const char *src_path(void)
{
    return (g_src_fixture || g_src_done) ? g_src_final : g_src_part;
}

static int src_range_read(long long off,void *buf,int bytes,long long total) {
    if(g_range_lock<0||g_range_disabled||!g_src_url[0]||g_src_fixture)return 0;
    for(;;){if((g_src_cancel||g_src_interrupted))return -1;SceUInt wait=100000;if(sceKernelWaitSema(g_range_lock,1,&wait)>=0)break;}
    if((g_src_cancel||g_src_interrupted)){sceKernelSignalSema(g_range_lock,1);return -1;}
    int result=0;
    if(off>=g_range_start&&off-g_range_start<g_range_len){
        int n=g_range_len-(int)(off-g_range_start);if(n>bytes)n=bytes;
        memcpy(buf,g_range_buf+(int)(off-g_range_start),n);result=n;goto finished;
    }
    if(off>=g_range_previous_start&&off-g_range_previous_start<g_range_previous_len){
        int n=g_range_previous_len-(int)(off-g_range_previous_start);if(n>bytes)n=bytes;
        memcpy(buf,g_range_previous+(int)(off-g_range_previous_start),n);result=n;goto finished;
    }
    if(g_range_disabled)goto finished;
    if(g_range_len>0){memcpy(g_range_previous,g_range_buf,g_range_len);g_range_previous_start=g_range_start;g_range_previous_len=g_range_len;}
    g_range_busy=1;
    char err[80]={0};long long end=off+sizeof(g_range_buf)-1;
    if(end>=total)end=total-1;
    if(http_open_range(&g_range_http,g_src_url,off,end,1500,err,sizeof(err))<0){http_close(&g_range_http);goto range_failed;}
    if(g_range_http.status!=206||g_range_http.range_start!=off||g_range_http.range_end!=end||g_range_http.range_total!=total||g_range_http.content_length!=end-off+1){http_close(&g_range_http);goto range_failed;}
    g_range_len=0;g_range_start=off;
    if(g_src_segmented)mlog("mv stream: RANGE segment=%lld..%lld prefix=%lld",off,end,mv_stream_available());
    http_set_timeout(&g_range_http,100);
    unsigned begin=sceKernelGetSystemTimeLow();
    while(!(g_src_cancel||g_src_interrupted)&&g_range_len<(int)(end-off+1)){
        int n=http_read(&g_range_http,(char*)g_range_buf+g_range_len,(int)(end-off+1)-g_range_len,100,&g_src_cancel,0,err,sizeof(err));
        if(n>0)g_range_len+=n;else if(n!=-1)break;
        if((unsigned)(sceKernelGetSystemTimeLow()-begin)>4000000u)break;
    }
    http_close(&g_range_http);
    if((g_src_cancel||g_src_interrupted)){result=-1;goto finished;}
    if(g_range_len){int n=g_range_len<bytes?g_range_len:bytes;memcpy(buf,g_range_buf,n);result=n;goto finished;}
range_failed:
    g_range_disabled=1;
    mlog("mv stream: Range unavailable status=%d got_start=%lld want=%lld bytes=%d error=%s; waiting for download",g_range_http.status,g_range_http.range_start,off,g_range_len,err);
finished:
    g_range_busy=0;
    sceKernelSignalSema(g_range_lock,1);return result;
}

static int src_read_at(void *opaque, int64_t off, void *buf, int bytes)
{
    unsigned t0 = sceKernelGetSystemTimeLow();
    (void)opaque;
    if(!buf)return -1;
    int amount;
    for(;;){
        MvSourceSnapshot v=src_snapshot();
        amount=mv_stream_read_window(off,bytes,v.available,v.total,v.done,v.error,v.cancel);
        if(amount!=MV_STREAM_READ_WAIT)break;
        if(!v.cancel&&!v.error&&(mv_stream_needs_range(off,v.available,v.total)||(g_src_segmented&&off>=v.available&&off<v.total))){
            int ranged=src_range_read(off,buf,bytes,v.total);if(ranged)return ranged;
        }
        if((unsigned)(sceKernelGetSystemTimeLow()-t0)>20000000u){
            mlog("mv stream: read timeout off=%lld need=%d avail=%lld",(long long)off,bytes,v.available);return -1;
        }
        sceKernelDelayThread(2000);
    }
    if(amount<=0)return amount;
    if(off>=0&&off<=g_src_avail&&off+amount<=g_src_avail){
        if(off<=g_src_read_cursor+128*1024&&off+amount>g_src_read_cursor)g_src_read_cursor=off+amount;
    }
    SceUID fd;
    for(;;){
        fd=sceIoOpen(src_path(),PSP_O_RDONLY,0);
        if(fd>=0)break;
        if(g_src_done){fd=sceIoOpen(g_src_final,PSP_O_RDONLY,0);break;}
        /* A high-priority reader may run after rename but before done=1.
         * Wait for this generation's publication; never read an old final. */
        if(!g_src_renaming||g_src_cancel||g_src_err){mlog("mv stream: read open failed result=0x%08X offset=%lld",fd,(long long)off);return -1;}
        if((unsigned)(sceKernelGetSystemTimeLow()-t0)>20000000u)return -1;
        sceKernelDelayThread(2000);
    }
    if(fd<0)return -1;
    int n=-1;
    if(sceIoLseek(fd,(SceOff)off,PSP_SEEK_SET)>=0)n=sceIoRead(fd,buf,amount);
    int closed=sceIoClose(fd);
    if(n<=0||closed<0)return -1; /* Published bytes must exist; never fake EOF. */
    return n;
}

static void src_interrupt_reads(void *opaque,int interrupted){(void)opaque;g_src_interrupted=interrupted!=0;}

static void src_cancel(void *opaque)
{
    (void)opaque;
    g_src_cancel = 1;
}

void mv_stream_abort(void){src_cancel(NULL);}
int mv_stream_cancelled(void){return g_src_cancel!=0;}

static int src_resolve_path(char*out,size_t capacity,const char*path){
 int n;if(!path||!path[0])return -1;
 if(strchr(path,':')||path[0]=='/')n=snprintf(out,capacity,"%s",path);
 else{char root[512];if(!getcwd(root,sizeof(root)))return -1;const char*relative=path;if(relative[0]=='.'&&relative[1]=='/')relative+=2;n=snprintf(out,capacity,"%s/%s",root,relative);}
 return n<0||(size_t)n>=capacity?-1:0;
}
int mv_stream_start(const char *url, const char *final_path)
{
    if(g_faulted || g_thread>=0)return -1;
    if (!url || !url[0] || !final_path || !final_path[0]) return -1;
    if (g_src_running) return -1;
    if (g_src_thid < 0) {
        mlog("mv stream: late worker reservation fallback; startup prepare missing/failed");
        int prepared = mv_stream_prepare_worker();
        if (prepared < 0) return prepared;
    }
    g_range_start=0;g_range_len=0;g_range_disabled=0;g_range_previous_start=0;g_range_previous_len=0;
    g_src_fixture=0;g_src_first_frame=0;g_src_renaming=0;g_src_interrupted=0;g_src_buffering=0;g_src_read_cursor=0;g_src_segmented=0;g_range_busy=0;
    g_src_mvid=0;g_src_name[0]=g_src_artist[0]=0;g_src_duration_ms=0;
    const char*filename=strrchr(final_path,'/');filename=filename?filename+1:final_path;
    if(sscanf(filename,"mv_%d_240.mp4",&g_src_mvid)!=1)g_src_mvid=0;
    const MvInfo*info=api_mv_info();
    if(g_src_mvid>0&&info->id==g_src_mvid){snprintf(g_src_name,sizeof(g_src_name),"%s",info->name);snprintf(g_src_artist,sizeof(g_src_artist),"%s",info->artist);g_src_duration_ms=info->dur_ms;}
    else snprintf(g_src_name,sizeof(g_src_name),"MV %d",g_src_mvid);
    snprintf(g_src_url, sizeof(g_src_url), "%s", url);
    if(src_resolve_path(g_src_final,sizeof(g_src_final),final_path)<0)return -1;
    int part_length=snprintf(g_src_part,sizeof(g_src_part),"%s.part",g_src_final);
    if(part_length<0||part_length>=(int)sizeof(g_src_part))return -1;
    g_src_avail = 0;
    g_src_total = 0;
    g_src_done = 0;
    g_src_err = 0;
    g_src_cancel = 0;
    return src_queue_session();
}

void mv_stream_duration(int milliseconds){g_src_duration_ms=milliseconds>0?milliseconds:0;}
long long mv_stream_prefetch(void){
    MvSourceSnapshot v=src_snapshot();long long target=512*1024;
    if(v.total>0&&g_src_duration_ms>0)target=v.total*mv_buffer_seconds()*1000/g_src_duration_ms;
    if(target<256*1024)target=256*1024;
    if(target>4*1024*1024)target=4*1024*1024;
    return v.total>0&&target>v.total?v.total:target;
}
int mv_stream_buffering(void){
    if(!g_use_source)return 0;
    MvSourceSnapshot v=src_snapshot();if(v.cancel||v.error||v.done){g_src_buffering=0;return 0;}
    /* A finished64KiB Range request is not proof of sustainable playback.
     * Keep the same continuous on-disk high-water gate after seeking. */
    if(g_src_segmented&&g_range_busy){g_src_buffering=1;return 1;}
    long long consumed=g_src_read_cursor;
    if(g_dur_ms>0&&g_pos_ms>0&&v.total>0){long long played=v.total*g_pos_ms/g_dur_ms;if(played>consumed)consumed=played;}
    long long ahead=v.available-consumed,target=mv_stream_prefetch();
    /* Resume only once a real buffer exists again. The old target/3 low-water
     * mark made a slow link restart playback every few seconds (visible stutter). */
    long long low=target/2;if(low<128*1024)low=128*1024;
    int was=g_src_buffering;
    if(!g_src_buffering&&ahead<low)g_src_buffering=1;
    if(g_src_buffering&&ahead>=target)g_src_buffering=0;
    if(was!=g_src_buffering)mlog("mv stream: BUFFER %s segmented=%d prefix=%lld cursor=%lld ahead=%lld target=%lld",g_src_buffering?"wait":"ready",(int)g_src_segmented,v.available,consumed,ahead,target);
    return g_src_buffering;
}
static void src_seek_begin(void*opaque,int64_t position_ms){
    (void)opaque;g_src_segmented=1;g_src_buffering=1;
    g_pos_ms=position_ms>0?(int)position_ms:0;
    int duration=g_dur_ms>0?g_dur_ms:g_src_duration_ms;
    long long total=mv_stream_total();
    g_src_read_cursor=duration>0&&total>0?total*g_pos_ms/duration:0;
    if(g_src_read_cursor>total&&total>0)g_src_read_cursor=total;
    mlog("mv stream: SEEK target=%lldms segmented_range=1 prefix=%lld/%lld",(long long)position_ms,mv_stream_available(),mv_stream_total());
}
static int src_buffering_callback(void*opaque){(void)opaque;return mv_stream_buffering();}

long long mv_stream_available(void) { return src_snapshot().available; }
long long mv_stream_total(void) { return src_snapshot().total; }
int mv_stream_active(void) { return g_src_running && !g_src_err; }
int mv_stream_worker_active(void) { return g_src_running != 0; }
int mv_stream_done(void) { return g_src_done; }
int mv_stream_error(void) { return g_src_err; }
/* Bytes per second actually received over the last 2s window (0 = unknown). */
long long mv_stream_rate_bps(void) { return g_src_rate_bps; }
/* Bytestream the video itself needs to play in real time; 0 when unknown. */
long long mv_stream_required_bps(void)
{
    long long total=src_snapshot().total;
    int duration=g_src_duration_ms>0?g_src_duration_ms:g_dur_ms;
    if(total<=0||duration<=0)return 0;
    return total*1000/duration;
}
/* Can this link sustain streaming? A measured rate below ~1.25x the bitrate
 * spends most of the video in the buffer-wait loop instead of playing. */
int mv_stream_sustainable(void)
{
    long long rate=g_src_rate_bps,need=mv_stream_required_bps();
    if(rate<=0||need<=0)return 1;         /* no measurement yet: don't block */
    return rate>=need+need/4;
}
long long mv_stream_eta_ms(void)
{
    long long total=src_snapshot().total,available=src_snapshot().available;
    long long rate=g_src_rate_bps;
    if(total<=0||rate<=0||available>=total)return 0;
    return (total-available)*1000/rate;
}
const char *mv_stream_part_path(void) { return g_src_part; }

static int mv_begin(const char *path, const char *title, int use_source);

static int settings_loaded, setting_decoder, setting_quality, setting_stream,setting_resolution,setting_buffer;
static int session_native;
static void mv_settings_load(void) {
    if(settings_loaded)return;
    setting_decoder=mv_decoder_value(mv_cfg_int(MV_DECODER_FILE,mv_cfg_int("mvavc.txt",0)));
    setting_quality=mv_cfg_int(MV_SOFT_QUALITY_FILE,mv_cfg_int("mvfast.txt",0));
    if(setting_quality<0||setting_quality>3)setting_quality=0;
    setting_stream=mv_cfg_int(MV_STREAM_FILE,1)==1;
    setting_resolution=mv_cfg_int("mvresolution.txt",0);
    if(setting_resolution!=240&&setting_resolution!=360&&setting_resolution!=480)setting_resolution=0;
    setting_buffer=mv_cfg_int("mvbuffer.txt",8);
    if(setting_buffer!=3&&setting_buffer!=8&&setting_buffer!=15&&setting_buffer!=30)setting_buffer=8;
    settings_loaded=1;
}
static int mv_settings_write(const char *name,int value,int *setting) {
    char path[96],text[4];mv_settings_load();
    snprintf(path,sizeof(path),APP_DIR "%s",name);
    int n=snprintf(text,sizeof(text),"%d\n",value);
    int fd=sceIoOpen(path,PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0600);
    if(fd<0)return -1;
    int written=sceIoWrite(fd,text,n),closed=sceIoClose(fd);
    if(written!=n||closed<0)return -1;
    *setting=value;return 0;
}
int mv_decoder_mode(void){mv_settings_load();return setting_decoder;}
int mv_soft_quality(void){mv_settings_load();return setting_quality;}
int mv_stream_enabled(void){mv_settings_load();return setting_stream;}
int mv_set_decoder_mode(int mode){if(mode<0||mode>2)return -1;return mv_settings_write(MV_DECODER_FILE,mode,&setting_decoder);}
int mv_set_soft_quality(int quality){if(quality<0||quality>3)return -1;return mv_settings_write(MV_SOFT_QUALITY_FILE,quality,&setting_quality);}
int mv_set_stream_enabled(int enabled){if(enabled<0||enabled>1)return -1;return mv_settings_write(MV_STREAM_FILE,enabled,&setting_stream);}
int mv_resolution(void){mv_settings_load();return setting_resolution;}
int mv_download_resolution(void){int manual=mv_resolution();if(manual)return manual;int speed=mv_cfg_int("mvbandwidth.txt",0),fps=mv_cfg_int("mvfps.txt",0);return speed>=256&&(fps>=24||mv_decoder_mode()==1)?360:240;}
int mv_buffer_seconds(void){mv_settings_load();return setting_buffer;}
int mv_set_resolution(int v){if(v!=0&&v!=240&&v!=360&&v!=480)return -1;return mv_settings_write("mvresolution.txt",v,&setting_resolution);}
int mv_set_buffer_seconds(int v){if(v!=3&&v!=8&&v!=15&&v!=30)return -1;return mv_settings_write("mvbuffer.txt",v,&setting_buffer);}
int mv_native_requested(void) { return mv_decoder_mode()==MV_DECODE_HARDWARE; }

int mv_start_stream(const char *final_path, const char *title)
{
    if(g_faulted || g_thread>=0)return -1;
    g_use_source = 1;
    g_source.size = mv_stream_total();
    g_source.read_at = src_read_at;
    g_source.cancel = src_cancel;
    g_source.first_frame = src_first_frame;
    g_source.interrupt_reads = src_interrupt_reads;
    g_source.buffering = src_buffering_callback;
    g_source.seek_begin = src_seek_begin;
    return mv_begin(final_path, title, 1);
}

static int mv_thread(SceSize args, void *argp)
{
#if defined(__PSP__)
    melody_bind_thread_directory();
#endif

    PspVideoOptions opt;
    PspVideoResult res;

    (void)args;
    (void)argp;
    memset(&opt, 0, sizeof(opt));
    memset(&res, 0, sizeof(res));
    if (g_use_source) {
        opt.path = NULL;
        opt.source = &g_source;
    } else {
        opt.path = g_path;
    }
    char module_path[768],module_logs[768],current_root[512];
    if(getcwd(current_root,sizeof(current_root))){
        if(opt.path&&opt.path[0]=='.'&&opt.path[1]=='/'){snprintf(module_path,sizeof(module_path),"%s/%s",current_root,opt.path+2);opt.path=module_path;}
        snprintf(module_logs,sizeof(module_logs),"%s/cache",current_root);opt.log_root=module_logs;
    }else opt.log_root="./cache";
    opt.backend = PSP_VIDEO_BACKEND_NATIVE_ME;
    opt.rgb_buffer_frames = 2;   /* PSP-1000 只有 32MB，2 帧 RGB 队列 = 1.1MB */
    opt.presenter = &g_presenter;
    opt.cancel_button = PSP_VIDEO_CANCEL_CROSS;   /* 用户要求：X = 返回 */

    /* 真实进入 PRX 前的余量（这里已经是分给解码器用的那一刻） */
    mlog("mv: prx enter free=%dK max=%dK",
         (int)(sceKernelTotalFreeMemSize() / 1024),
         (int)(sceKernelMaxFreeMemSize() / 1024));
    log_flush();     /* 崩在 PRX 里时这一行必须已经在卡上 */
    g_result = (g_host && g_host->mv_play) ? g_host->mv_play(&opt, &res) : -1;
    mlog("mv: r=%d err=%d frames=%u eof=%u pos=%lld free=%dK", g_result, res.error,
         res.frames, res.eof, (long long)res.position_ms,
         (int)(sceKernelTotalFreeMemSize() / 1024));
    mlog("mv: completed seeks=%u final_position=%lld frames=%u",res.seeks,(long long)res.position_ms,res.frames);
    g_finished = 1;
    return 0;
}

int mv_start(int mvid, const char *path, const char *title)
{
    if(g_faulted || g_thread>=0)return -1;
    /* 播放本地已存在的 MV 时顺手补进索引（覆盖"索引引入之前就下好"的老文件） */
    if (mvid > 0 && path && path[0]) {
        SceIoStat st;

        if (sceIoGetstat(path, &st) >= 0 && st.st_size > 0)
            mv_index_add(mvid, (int)st.st_size);
    }
    if(mvid==990000002 && mv_native_requested()){
        if(g_thread>=0 || g_src_running)return -1;
        SceIoStat st;if(sceIoGetstat(path,&st)<0||st.st_size<=0)return -1;
        if(src_resolve_path(g_src_final,sizeof(g_src_final),path)<0)return -1;
        unsigned irq=sceKernelCpuSuspendIntr();
        g_src_fixture=1;g_src_fixture_epoch=sceKernelGetSystemTimeWide();
        g_src_total=st.st_size;g_src_avail=0;g_src_done=g_src_err=g_src_cancel=0;g_src_first_frame=0;
        sceKernelCpuResumeIntr(irq);
        mlog("mv stream: local rate fixture256KiB initial +128KiB/s; NOT a network speed test");
        return mv_start_stream(path,title);
    }
    return mv_begin(path, title, 0);
}

static int mv_select_session(const char*path,int streaming){
    int mode=mv_decoder_mode();session_native=mode==1;MvProbe probe={0};
    if(mode==2){
        static unsigned char bytes[32768];long long total=streaming?mv_stream_total():0;
        SceUID fd=-1;if(!streaming){fd=sceIoOpen(path,PSP_O_RDONLY,0);if(fd>=0)total=sceIoLseek(fd,0,PSP_SEEK_END);}
        long long offsets[2]={0,total>32768?total-32768:0};
        for(int i=0;i<2;++i){int n=0;
            if(streaming)n=src_read_at(NULL,offsets[i],bytes,sizeof(bytes));
            else if(fd>=0&&sceIoLseek(fd,offsets[i],PSP_SEEK_SET)>=0)n=sceIoRead(fd,bytes,sizeof(bytes));
            if(n>0)mv_probe_bytes(&probe,bytes,n);
        }if(fd>=0)sceIoClose(fd);
        session_native=mv_probe_hardware(&probe);
        if(session_native){SceIoStat st;if(sceIoGetstat(APP_DIR "avc_firmware_load.prx",&st)<0||sceIoGetstat(APP_DIR "melody_mv_avc.prx",&st)<0){session_native=0;mlog("mv auto: hardware module unavailable, selecting software before hardware init");}}
        mlog("mv auto: profile=%d level=%d %dx%d selected=%s",probe.profile,probe.level,probe.width,probe.height,session_native?"hardware":"software");
    }
    int ignored=0;return mv_settings_write("mvsession.txt",session_native,&ignored);
}

static int mv_begin(const char *path, const char *title, int use_source)
{
    mv_capture_palette(); /* UI thread snapshot: video callbacks never touch skin VM. */
    if (g_faulted) {
        mlog("mv: previous session failed to stop, restart required");
        return -1;
    }
    if (g_thread >= 0) return -1;
    if (!path || !path[0]) return -1;

    g_use_source = use_source ? 1 : 0;
    if(mv_select_session(path,g_use_source)<0){mlog("mv: cannot save session decoder selection");return -1;}
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = 0;
    g_title[0] = 0;
    if (title) {
        strncpy(g_title, title, sizeof(g_title) - 1);
        g_title[sizeof(g_title) - 1] = 0;
    }
    g_pos_ms = 0;
    g_dur_ms = 0;
    g_quit = 0;
    g_frame_ready = 0;
    g_finished = 0;
    g_result = -1;
    g_last_frame_us = 0;
    g_wd_last_frames = 0;
    g_wd_frames_us = 0;
    g_got_frame = 0;
    g_frames = 0;
    g_frames_t0 = 0;
    g_present_timeouts = 0;
    g_perf_frames = g_copy_us_max = g_post_us_max = 0;
    g_copy_us_total = g_post_us_total = 0;
    g_beat_us = 0;
    g_pos_us = 0;
    g_pos_last = 0;
    g_start_us = (unsigned)sceKernelGetSystemTimeLow();

    if (!g_host || !g_host->mv_load) {
        mlog("mv: no host loader");
        return -1;
    }
    /* 先读文件开关：路标日志要用 g_diag；这两个都是很小的 ms0: 读，
     * 顺序放在任何等待之前，卡住时「没有路标」本身就是结论。 */
    g_diag = mv_cfg_int("mvdiag.txt", 0) != 0;
    g_keep_loaded = mv_cfg_int("mvkeep.txt", 0) != 0;
    g_mv_dma = mv_cfg_int("mvdma.txt", 1) != 0;
    g_osd_enable = mv_cfg_int("mvosd.txt", 1) != 0;
    /* present 等主线程取帧的总截止：mvpresent.txt 写毫秒数可调（默认 500）。
     * 夹在 100..5000：太小会误丢帧，太大就等于回到「卡住也不动」的老问题；
     * 写成 0/负数按默认处理，避免手滑变成「每一帧都超时」。 */
    g_present_wait_ms = (unsigned)mv_cfg_int("mvpresent.txt", 500);
    if (g_present_wait_ms < 100u) g_present_wait_ms = 500u;
    if (g_present_wait_ms > 5000u) g_present_wait_ms = 5000u;
    mv_step("begin");
    mv_status("正在释放内存", 15);
    mv_step("status");
    /* Pause only stops PCM output. Retire MP3 on its output owner before
     * loading/booting any native MV decoder or releasing API resources. */
    if (audio_mv_suspend() < 0) {
        mlog("mv: MP3 retirement unconfirmed, native start blocked");
        return -1;
    }
    g_audio_suspended = 1;
    mv_step("audio-retired");
    /* 每次进 MV 都要停掉 api 线程并释放那 1.3MB 缓冲：
     * 之前用 s_api_released 门闩只做一次，第二次进 MV 时缓冲还占着，
     * 565 的 RAM 帧可能分配失败 → 永远不出画面。 */
    {
        /* api 线程正跑着请求（取流/封面/评论，备用音源最坏 12 秒）时，
         * api_shutdown() 3 秒等不到就强杀它。被强杀的线程可能还停在某个
         * 内核调用里、并且正握着 g_lock/io_lock —— 它一死锁就再也回不来，
         * 主线程下一句 api_clear_events()/mlog() 就永久卡死（PPSSPP 上
         * 「菜单停在播放 MV、界面冻住」的机制）。api_shutdown() 现在会在
         * 强杀后重建那两把锁；这里再多等 100ms，让它真正退出后再 free
         * 它正在用的 1.3MB 缓冲，避免「释放别人还在写的内存」。 */
        int forced = api_shutdown();

        mv_step("api-shutdown");
        if (forced) {
            mlog("mv: api thread force-killed, locks recreated, wait before free");
            sceKernelDelayThread(100 * 1000);
            mv_step("api-forced");
        }
    }
    api_clear_events();
    mv_step("events-cleared");
    update_gallery_shutdown();
    gfx_suspend_renderer();
    ui_free_skin_preview();
    mv_step("preview-freed");
    skin_runtime_suspend();
    mv_step("skin-freed");
    api_free_buffers();
    mv_step("buffers-freed");
    s_api_released = 1;

    /* ---- 起播内存闸门（真机第二次进 MV 重启的防线）---- */
    sysi_update();                 /* 必须现取：以前 mv: pre 打的是上一帧的旧值 */
    g_block_reason = 0;
    {
        int def_need = g_prx_loaded ? MV_MIN_FREE_LOADED_KB : MV_MIN_FREE_FIRST_KB;
        int need = mv_cfg_int("mvminfree.txt", def_need);
        int force = mv_cfg_int("mvforce.txt", 0) != 0;
        int free_kb = sysi_free_kb();
        int max_kb = sysi_maxblock_kb();

        mlog("mv: gate #%d free=%dK max=%dK need=%dK prx=%d force=%d diag=%d keep=%d dma=%d",
             g_sessions + 1, free_kb, sysi_maxblock_kb(), need, g_prx_loaded, force,
             g_diag, g_keep_loaded, g_mv_dma);
        if (g_diag) mv_memory_diag();
        if (mv_memory_gate_blocks(free_kb, max_kb, need, force)) {
            /* PRX 的 12.9MB 没还回来时只剩 ~2MB：进去就是重启，这里直接拒绝。
             * main.c 用 mv_block_reason() 弹「内存不足，请重启后再看 MV」。 */
            g_block_reason = 1;
            mlog("mv: blocked, free=%dK max=%dK need=%dK cause=%s (session #%d)",
                 free_kb, max_kb, need, free_kb < need ? "total" : "fragmented",
                 g_sessions + 1);
            if (!g_diag) mv_memory_diag();
            api_init();
            mv_restore_audio();
            return -1;
        }
    }

    mv_step("gate-passed");
    mv_status("正在加载 MV 解码器", 35);
    mlog("mv: pre free=%dK max=%dK batt=%d%% chg=%d online=%d temp=%d",
         sysi_free_kb(), sysi_maxblock_kb(), scePowerGetBatteryLifePercent(),
         scePowerIsBatteryCharging(), scePowerIsPowerOnline(), scePowerGetBatteryTemp());
    mv_step("prx-request");
    int loaded=g_host->mv_load();
    if (loaded < 0) {
        if(loaded==-6)g_block_reason=2;
        mlog("mv: melody_mv.prx load failed");
        mv_step("prx-failed");
        api_init();
        mv_restore_audio();
        mv_status("MV 解码器加载失败", 100);
        return -1;
    }
    g_prx_loaded = 1;
    mv_step("prx-loaded");
    /* 装载之后再看一眼：第一次加载会吃掉约 4.7MB 模块镜像（8MB 堆是懒分配的） */
    sysi_update();
    mlog("mv: loaded free=%dK max=%dK", sysi_free_kb(), sysi_maxblock_kb());
    mv_status("准备播放", 80);
    mv_step("gfx-begin-enter");
    gfx_begin_direct_video();
    mv_step("gfx-begin-done");

    g_ready = sceKernelCreateSema("mv_ready", 0, 0, 1, NULL);
    g_taken = sceKernelCreateSema("mv_taken", 0, 0, 1, NULL);
    if (g_ready < 0 || g_taken < 0) {
        if (g_ready >= 0) sceKernelDeleteSema(g_ready);
        if (g_taken >= 0) sceKernelDeleteSema(g_taken);
        g_ready = g_taken = -1;
        if (!g_host->mv_unload || g_host->mv_unload() < 0) {
            g_faulted = 1;
            mlog("mv: start cleanup unload unconfirmed; MP3 retained");
            return -1;
        }
        g_prx_loaded = 0;
        gfx_end_direct_video();
        api_init();
        mv_restore_audio();
        return -1;
    }
    g_thread = sceKernelCreateThread("mv_play", mv_thread, 0x17, 0x40000,
                                     PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (g_thread < 0) {
        sceKernelDeleteSema(g_ready);
        sceKernelDeleteSema(g_taken);
        g_ready = g_taken = -1;
        if (!g_host->mv_unload || g_host->mv_unload() < 0) {
            g_faulted = 1;
            mlog("mv: start cleanup unload unconfirmed; MP3 retained");
            return -1;
        }
        g_prx_loaded = 0;
        gfx_end_direct_video();
        api_init();
        mv_restore_audio();
        return -1;
    }
    if (sceKernelStartThread(g_thread, 0, NULL) < 0) {
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
        sceKernelDeleteSema(g_ready);
        sceKernelDeleteSema(g_taken);
        g_ready = g_taken = -1;
        if (!g_host->mv_unload || g_host->mv_unload() < 0) {
            g_faulted = 1;
            mlog("mv: start cleanup unload unconfirmed; MP3 retained");
            return -1;
        }
        g_prx_loaded = 0;
        gfx_end_direct_video();
        api_init();
        mv_restore_audio();
        return -1;
    }
    mv_step("thread-started");
    ++g_sessions;
    mlog("mv: start #%d src=%d %s", g_sessions, g_use_source, g_path);
    return 0;
}

int mv_display_tick(void)
{
    scePowerTick(PSP_POWER_TICK_ALL);

    /* 心跳：每 2 秒一行 + 立刻刷盘。真机卡死那次 log.txt 的最后一行是
     * 「tick frame=7200」，MV 里一行 mv: 都没留下 —— 因为 SC_MV 期间
     * 不 flush，最后几十行全丢在 stdio 缓冲里（docs/nc0-corruption.md §7）。
     * 这一行保证任何一次硬停之后，log.txt 末尾都是「卡住前 2 秒的实况」。 */
    if (g_thread >= 0 && !g_finished) {
        unsigned now = (unsigned)sceKernelGetSystemTimeLow();

        if (g_frames != g_wd_last_frames) {
            g_wd_last_frames = g_frames;
            g_wd_frames_us = now;
        }
        if ((unsigned)(now - g_beat_us) >= 2000000u) {
            g_beat_us = now;
            mlog("mv: alive f=%d pos=%d free=%d", g_frames, g_pos_ms,
                 (int)(sceKernelTotalFreeMemSize() / 1024));
            log_flush();
        }
    }

    /* ---- 整段 MV 进度看门狗（不依赖 g_frame_ready）----
     * 下面那条 6 秒「没新帧」只在 g_frame_ready==0 的分支里检查；主循环要是
     * 被别的东西占住、或者上一帧一直没被取走（g_frame_ready 卡在 1），它就
     * 永远不跑 —— 真机那次的 mvstep.txt 停在 thread-started，既没有
     * 「no frame for 6s」也没有「mv: r=」，正是这种「看门狗根本没执行」。
     * 这里每次 tick 都查，两个判据（任一成立就停）：
     *   1. 本地文件 8 秒 / 边下边播 15 秒没出新帧（真死）；
     *   2. 播放位置 15 秒没前进（时钟停了，画面也就不动了）。
     *      只有「已知时长 + 位置确实动过」才判，避免纯视频（没有音轨、
     *      position 恒为 0）被误停。
     * 停完 g_quit=1，main.c 的 mv_finished()->mv_join() 会回播放页。 */
    if(g_thread>=0&&g_use_source&&mv_stream_buffering()){
        unsigned now=sceKernelGetSystemTimeLow();g_wd_frames_us=now;g_pos_us=now;
    }
    if (g_thread >= 0 && !g_finished && !g_quit && g_got_frame &&
        !g_mv_paused && !g_mv_seeking && !(g_use_source&&mv_stream_buffering())) {
        unsigned now = (unsigned)sceKernelGetSystemTimeLow();
        /* 12 秒没有新帧（本地）/ 20 秒（边下边播）：追帧重建 GOP 要 4-9 秒，
         * 留足余量。用主线程自己记的 g_wd_frames_us，不用跨线程的
         * g_last_frame_us（真机日志证明它会在帧还在涨时被读成 6 秒旧值）。 */
        unsigned noframe = g_use_source ? 20000000u : 12000000u;

        if (g_pos_ms != g_pos_last) {
            g_pos_last = g_pos_ms;
            g_pos_us = now;
        }
        if (g_wd_frames_us && (unsigned)(now - g_wd_frames_us) > noframe) {
            mlog("mv: stalled pos=%d, stopping (no new frame %ds src=%d frames=%d)",
                 g_pos_ms, (int)(noframe / 1000000u), g_use_source, g_frames);
            mv_stop();
            g_wd_frames_us = now;
            g_pos_us = now;
        } else if (g_dur_ms > 0 && g_pos_last > 0 && g_pos_us &&
                   (unsigned)(now - g_pos_us) > 15000000u) {
            mlog("mv: stalled pos=%d, stopping (clock 15s src=%d)", g_pos_ms, g_use_source);
            mv_stop();
            g_wd_frames_us = now;
            g_pos_us = now;
        }
    }

    /* 诊断：每 250ms 一行，真机重启后看最后一行就知道崩在第几秒。
     * 只用内核调用取内存（不走 FAT），默认关闭，mvdiag.txt=1 打开。 */
    if (g_diag && g_thread >= 0) {
        unsigned now = (unsigned)sceKernelGetSystemTimeLow();

        if (g_start_us && (unsigned)(now - g_diag_last) > 250000u) {
            g_diag_last = now;
            mlog("mv: alive t=%ums vc=%u free=%dK frames=%d pos=%dms got=%d fin=%d",
                 (unsigned)((now - g_start_us) / 1000), sceDisplayGetVcount(),
                 (int)(sceKernelTotalFreeMemSize() / 1024), g_frames, g_pos_ms,
                 g_got_frame, g_finished);
            log_flush();     /* 诊断模式下每行都落卡，重启后能看出崩在第几秒 */
        }
    }

    if (!g_frame_ready) {
        unsigned now = (unsigned)sceKernelGetSystemTimeLow();

        /* 还没有第一帧：普通文件 12 秒放弃；边下边播要给下载留时间（30 秒） */
        if (!g_got_frame && g_thread >= 0 && !g_finished && g_start_us && !(g_use_source&&mv_stream_buffering()) &&
            (unsigned)(now - g_start_us) > (g_use_source ? 30000000u : 12000000u)) {
            mlog("mv: no first frame in %ds, stop (src=%d)", g_use_source ? 30 : 12,
                 g_use_source);
            mv_stop();
            g_start_us = now;
        }
        return 0;
    }
    g_frame_ready = 0;
    /* 这一帧已经 DMA 进 scanout 了：放行解码线程去写下一帧 */
    g_wd_frames_us = sceKernelGetSystemTimeLow();
    if (g_taken >= 0) sceKernelSignalSema(g_taken, 1);
    return 1;
}

int mv_active(void)
{
    return g_thread >= 0 && !g_finished;
}

int mv_finished(void)
{
    return !g_faulted && g_thread >= 0 && g_finished;
}

int mv_join(void)
{
    int wedged = 0;
    int audio_result;

    if (g_faulted) return -1;
    mv_step("join-enter");
    if (g_thread >= 0) {
        /* 正常情况下线程早就 return 了；卡在解码里时最多等 10 秒
         * （25fps + 精确 qpel 时单帧可能要几十毫秒，3 秒太短，容易误判成
         * wedged 然后带着还在跑的线程回 UI）。 */
        SceUInt timeout = 10 * 1000 * 1000;
        int r = sceKernelWaitThreadEnd(g_thread, &timeout);

        if (r < 0) {
            /* 绝不能「强杀线程 + UnloadModule」：PRX 里的代码还在跑，
             * 卸载会把正在执行的代码抽走，真机上会立刻重启。 */
            mlog("mv: thread did not exit (0x%08X), wedged -> skip unload", r);
            wedged = 1;
        } else {
            sceKernelDeleteThread(g_thread);
            g_thread = -1;
        }
    }
    mv_step(wedged ? "join-wedged" : "thread-exited");
    /* MV57 explicitly marks any unconfirmed ME worker stop. This status
     * outranks ordinary decode/read errors and user cancellation; keep the
     * entire PRX/heap and forbid replay until App restart. */
    if (!wedged && mv_result_requires_restart(g_result)) {
        mlog("mv: me result %d, module kept (reboot needed)", g_result);
        wedged = 1;
    }
    /* 每次播完都卸载 MV PRX，把那 12.9MB（镜像 4.7MB + module_stop 里
     * __psp_free_heap() 归还的 8MB sbrk 堆）还给系统，下一次起播重新装载 ——
     * 这样每次起播的余量和第一次一样（真机 ~14.9MB），多听几次 MV 也不会重启。
     * 绝不带 wedged 卸载：PRX 里的代码还在跑，卸载会把正在执行的代码抽走。
     * mvkeep.txt=1 可以退回旧行为（常驻不卸载）做 A/B 对比。 */
    if (wedged) {
        g_faulted = 1;
        mlog("mv: wedged, module kept; restart required");
        /* A live presenter may still use App frame RAM/semaphores. Do not
         * free these or resume normal rendering underneath an active DMA. */
        return -1;
    } else if ((!g_keep_loaded || g_audio_suspended) && g_prx_loaded) {
        int before = (int)(sceKernelTotalFreeMemSize() / 1024);
        int after;

        mv_step("unload-request");
        if (!g_host || !g_host->mv_unload || g_host->mv_unload() < 0) {
            g_faulted = 1;
            mlog("mv: native unload unconfirmed; MP3 retained");
            return -1;
        }
        mv_step("unloaded");
        g_prx_loaded = 0;
        after = (int)(sceKernelTotalFreeMemSize() / 1024);
        mlog("mv: unloaded reclaimed=%dK free=%dK->%dK (满额约 12915K)",
             after - before, before, after);
    }
    mv_step("gfx-end-begin");
    gfx_end_direct_video();
    mv_step("gfx-end-done");
    mv_step("api-init-begin");
    api_init();
    audio_result = mv_restore_audio();
    mv_step("api-init-done");
    if (g_app.has_now && g_app.now.id > 0) api_request_art(&g_app.now);
    if (g_ready >= 0) { sceKernelDeleteSema(g_ready); g_ready = -1; }
    if (g_taken >= 0) { sceKernelDeleteSema(g_taken); g_taken = -1; }
    if(g_frames>=60&&g_frames_t0){unsigned elapsed=sceKernelGetSystemTimeLow()-g_frames_t0;if(elapsed>1000000u){int ignored=0;mv_settings_write("mvfps.txt",(int)((long long)g_frames*1000000/elapsed),&ignored);}}
    g_frame_ready = 0;
    g_finished = 0;
    g_got_frame = 0;
    g_last_frame_us = 0;
    g_present_max_us = 0;
    mv_step("join-done");
    /* The worker and native owner are retired. MP3 restore failure belongs
     * to audio; a negative join result would falsely retain the video UI. */
    if (audio_result < 0) mlog("mv: safe video retirement; MP3 resume failed r=%d", audio_result);
    return 0;
}

void mv_stop(void)
{
    if (g_thread < 0) return;
    g_quit = 1;
    if (g_use_source) g_src_cancel = 1;   /* 叫醒阻塞在 read_at 的线程 */
    if (g_host && g_host->mv_stop) g_host->mv_stop();
    if (g_taken >= 0) sceKernelSignalSema(g_taken, 1);
}

int mv_result(void)
{
    return g_result;
}

/* 起播被拒的原因：0 = 没被拒，1 = 可用内存不足（main.c 用它决定弹什么提示）。
 * 真机第二次进 MV 时 PRX 已常驻、只剩约 2MB，进去就是重启，所以起播前拦掉。 */
int mv_block_reason(void)
{
    return g_block_reason;
}

int mv_position_ms(void)
{
    return g_pos_ms;
}

/* ------------------------------------------------------------------ */
/* 下载页 MV 分类                                                     */
/* ------------------------------------------------------------------ */

#define MV_CACHE "./cache"
/* MV 列表索引（cache/mv_index.txt）
 * 一行：<mvid>\t<size>\t<name>\t<artist>\n
 *
 * 为什么不用 sceIoDread 扫目录：真机 FAT 在“大文件刚写完/正在写”时枚举不可靠
 * （只返回 "." 或者把 ms0: 驱动卡住，docs/C2-HANDOFF.md 记录过）。所以列表只读
 * 这个索引；索引的写入点只有三个：
 *   1. MV 下载完成（api.c，写完 sidecar 之后）；
 *   2. 播放任意本地 MV（mv_start，顺带把老文件补进索引）；
 *   3. 启动时如果索引不存在，扫一次目录做 bootstrap（此时没有下载/播放，FAT 空闲）。
 */
#define MV_INDEX_PATH  MV_CACHE "/mv_index.txt"
#define MV_INDEX_BYTES 6144
#define MV_SNAP_MAX    MV_DL_MAX

static MvDlItem g_snap[MV_SNAP_MAX];
static int g_snap_n;
static unsigned g_snap_last;
static int g_snap_state = -1;

static void mv_meta_read(int mvid, char *name, int nlen, char *artist, int alen, int *dur)
{
    char path[160], b[256];
    int fd, n;

    name[0] = 0;
    artist[0] = 0;
    if (dur) *dur = 0;
    snprintf(path, sizeof(path), MV_CACHE "/mv_%d_240.txt", mvid);
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    n = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    b[n] = 0;
    {
        char *p = strchr(b, '\n');
        char *q;

        if (!p) return;
        *p++ = 0;
        q = strchr(p, '\n');
        if (q) *q = 0;
        snprintf(name, nlen, "%s", b);
        snprintf(artist, alen, "%s", p);
        if (dur && q) *dur = atoi(q + 1);
    }
}

/* 解析一行 "<mvid>\t<size>\t<name>\t<artist>"（行尾的 \n 已由调用方去掉） */
static int mv_index_parse(char *line, int *mvid, int *size, char *name, int nlen,
                          char *artist, int alen)
{
    char *f1 = strchr(line, '\t');
    char *f2;
    char *f3;

    if (!f1) return 0;
    *f1++ = 0;
    f2 = strchr(f1, '\t');
    if (!f2) return 0;
    *f2++ = 0;
    f3 = strchr(f2, '\t');
    if (!f3) return 0;
    *f3++ = 0;
    *mvid = atoi(line);
    *size = atoi(f1);
    snprintf(name, nlen, "%s", f2);
    snprintf(artist, alen, "%s", f3);
    return *mvid > 0;
}

/* 重写索引：丢掉 skip_mvid 那一行，再按需追加 append 一行。 */
static void mv_index_store(int skip_mvid, const char *append)
{
    char in[MV_INDEX_BYTES], out[MV_INDEX_BYTES + 256];
    int fd, n, olen = 0;
    char *p;

    in[0] = 0;
    fd = sceIoOpen(MV_INDEX_PATH, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        n = sceIoRead(fd, in, sizeof(in) - 1);
        sceIoClose(fd);
        if (n <= 0) in[0] = 0;
        else in[n] = 0;
    }
    p = in;
    while (*p) {
        char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);

        if (len > 0) {
            int id = atoi(p);

            if (id > 0 && id != skip_mvid && olen + len + 1 < (int)sizeof(out)) {
                memcpy(out + olen, p, len);
                olen += len;
                out[olen++] = '\n';
            }
        }
        if (!e) break;
        p = e + 1;
    }
    if (append && append[0]) {
        int l = (int)strlen(append);

        if (olen + l < (int)sizeof(out)) {
            memcpy(out + olen, append, l);
            olen += l;
        }
    }
    fd=sceIoOpen(MV_INDEX_PATH ".tmp",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);
    if(fd<0)return;
    int okay=olen==0||sceIoWrite(fd,out,olen)==olen;
    if(sceIoClose(fd)<0)okay=0;
    if(!okay){sceIoRemove(MV_INDEX_PATH ".tmp");mlog("mv index: write failed; old index retained");return;}
    sceIoRemove(MV_INDEX_PATH ".bak");SceIoStat old_index;
    int had_old=sceIoGetstat(MV_INDEX_PATH,&old_index)>=0;
    if(had_old&&sceIoRename(MV_INDEX_PATH,MV_INDEX_PATH ".bak")<0){sceIoRemove(MV_INDEX_PATH ".tmp");return;}
    if(sceIoRename(MV_INDEX_PATH ".tmp",MV_INDEX_PATH)<0){if(had_old)sceIoRename(MV_INDEX_PATH ".bak",MV_INDEX_PATH);mlog("mv index: publish failed; prior index retained");return;}
    sceIoRemove(MV_INDEX_PATH ".bak");g_snap_last=0;
}

/* 供 api.c / mv_start 调用：把一支 MV 记进索引。名字从 sidecar 取。 */
void mv_index_add(int mvid, int size)
{
    char name[72] = "", artist[64] = "", line[256];
    int dur = 0;

    if (mvid <= 0 || size < 0) return;
    mv_meta_read(mvid, name, sizeof(name), artist, sizeof(artist), &dur);
    if (!name[0]) snprintf(name, sizeof(name), "MV %d", mvid);
    for (int i = 0; name[i]; ++i)
        if (name[i] == '\t' || name[i] == '\n' || name[i] == '\r') name[i] = ' ';
    for (int i = 0; artist[i]; ++i)
        if (artist[i] == '\t' || artist[i] == '\n' || artist[i] == '\r') artist[i] = ' ';
    snprintf(line, sizeof(line), "%d\t%d\t%s\t%s\n", mvid, size, name, artist);
    mv_index_store(mvid, line);
}

static void mv_index_remove(int mvid)
{
    if (mvid > 0) mv_index_store(mvid, NULL);
}

/* 读索引 → MvDlItem；文件不存在/大小为 0 的条目跳过。 */
static int mv_index_load(MvDlItem *out, int max)
{
    char buf[MV_INDEX_BYTES];
    int fd, n, count = 0;
    char *p;

    if (!out || max <= 0) return 0;
    fd = sceIoOpen(MV_INDEX_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    p = buf;
    while (*p && count < max) {
        char *e = strchr(p, '\n');
        int mvid = 0, size = 0;
        char name[72] = "", artist[64] = "";

        if (e) *e = 0;
        if (*p && mv_index_parse(p, &mvid, &size, name, sizeof(name), artist, sizeof(artist))) {
            char path[160];
            SceIoStat st;

            snprintf(path, sizeof(path), MV_CACHE "/mv_%d_240.mp4", mvid);
            /* "Complete" means at least the size the index recorded. A truncated
             * file used to be listed as finished until playback failed. */
            int complete=sceIoGetstat(path,&st)>=0&&st.st_size>0&&(size<=0||st.st_size>=size);
            if(!complete){int length=snprintf(path,sizeof(path),MV_CACHE "/mv_%d_240.mp4.part",mvid);if(length<0||length>=(int)sizeof(path)||sceIoGetstat(path,&st)<0||st.st_size<0)goto next_index_entry;}
            if(api_mv_download_state()==1&&api_mv_info()->id==mvid)goto next_index_entry;
            {
                MvDlItem *it = &out[count++];

                memset(it, 0, sizeof(*it));
                it->mvid = mvid;
                snprintf(it->name, sizeof(it->name), "%s", name[0] ? name : "MV");
                snprintf(it->artist, sizeof(it->artist), "%s", artist);
                if(strlen(path)>=sizeof(it->path)){--count;goto next_index_entry;}
                memcpy(it->path,path,strlen(path)+1);
                it->size = size>0?size:(int)st.st_size;
                it->state = complete?2:3;
                it->pct = complete?100:(size>0?(int)(st.st_size*100/size):0);
                if(!complete&&it->pct>99)it->pct=99;
            }
        }
next_index_entry:
        if (!e) break;
        p = e + 1;
    }
    /* 新到旧 */
    for (int i = 1; i < count; ++i) {
        MvDlItem key = out[i];
        int j = i - 1;

        while (j >= 0 && out[j].mvid < key.mvid) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = key;
    }
    return count;
}

/* 启动时调用一次：没有索引才扫目录 bootstrap。此时没有下载/播放，FAT 空闲；
 * 只认精确后缀 "_240.mp4"，跳过 .part 半截文件。 */
void mv_index_init(void)
{
    SceUID d;
    SceIoDirent de;
    char existing[MV_INDEX_BYTES];existing[0]=0;
    int fd,n=0,found=0,had_index=0;

    {SceIoStat stat;if(sceIoGetstat(MV_INDEX_PATH,&stat)<0)sceIoRename(MV_INDEX_PATH ".bak",MV_INDEX_PATH);}
    fd = sceIoOpen(MV_INDEX_PATH, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        n=sceIoRead(fd,existing,sizeof(existing)-1);
        sceIoClose(fd);
        if(n>0){existing[n]=0;had_index=1;}          /* 索引已存在，不扫目录 */
    }
    sceIoSync("ms0:", 0);
    d = sceIoDopen(MV_CACHE);
    if (d < 0) return;
    for (;;) {
        size_t len;
        int mvid = 0;
        char path[160];
        SceIoStat st;

        /* 必须每次清零：真机驱动不会把所有字段都写满，未初始化的 d_stat
         * 会让 st_size/st_mode 变成垃圾（第一次实现就是因此一条都没认到）。 */
        memset(&de, 0, sizeof(de));
        if (sceIoDread(d, &de) <= 0) break;
        if (!(de.d_stat.st_mode & FIO_S_IFREG)) continue;
        len = strlen(de.d_name);
        int partial=len>=13&&!strcasecmp(de.d_name+len-13,"_240.mp4.part");
        if(!partial&&(had_index||len<8||strcasecmp(de.d_name+len-8,"_240.mp4")))continue;
        if(sscanf(de.d_name,"mv_%d_240.mp4",&mvid)!=1||mvid<=0)continue;
        if(partial&&had_index){char needle[40];snprintf(needle,sizeof(needle),"%d\t",mvid);size_t size=strlen(needle);int known=0;for(const char*row=existing;row&&*row;){if(!strncmp(row,needle,size)){known=1;break;}row=strchr(row,'\n');if(row)row++;}if(known)continue;}
        /* 大小以 getstat 为准，别信 dirent 里的 d_stat */
        snprintf(path, sizeof(path), MV_CACHE "/%s", de.d_name);
        if (sceIoGetstat(path, &st) < 0 || st.st_size <= 0) continue;
        mv_index_add(mvid,partial?0:(int)st.st_size);
        ++found;
    }
    sceIoDclose(d);
    mlog("mv index: bootstrapped %d entries", found);
}

static MvDlItem local_videos[16];
static int local_video_count,local_videos_scanned;
/* Local VIDEO scanning is opt-in (settings -> 本地 MV 搜索). The list is meant to
 * mirror what this app downloaded; a large VIDEO folder costs a directory walk
 * and may list files the built-in decoder cannot open. */
void mv_local_video_rescan(void){local_videos_scanned=0;local_video_count=0;g_snap_last=0;}
static void mv_scan_local_videos(void){
    if(local_videos_scanned)return;local_videos_scanned=1;
    local_video_count=0;
    if(!store_scan_local_mv())return;
    const char *dirs[]={"ms0:/VIDEO","ms0:/PSP/VIDEO"};
    for(unsigned dir=0;dir<2&&local_video_count<16;++dir){
        SceUID d=sceIoDopen(dirs[dir]);if(d<0)continue;
        for(;;){SceIoDirent de;memset(&de,0,sizeof(de));if(sceIoDread(d,&de)<=0)break;
            const char *ext=strrchr(de.d_name,'.');
            if(!ext||(strcasecmp(ext,".mp4")&&strcasecmp(ext,".m4v")&&strcasecmp(ext,".3gp")&&strcasecmp(ext,".flv")&&strcasecmp(ext,".avi")&&strcasecmp(ext,".mkv")))continue;
            MvDlItem *it=&local_videos[local_video_count];memset(it,0,sizeof(*it));
            int length=snprintf(it->path,sizeof(it->path),"%s/%s",dirs[dir],de.d_name);if(length<0||length>=(int)sizeof(it->path))continue;
            SceIoStat st;if(sceIoGetstat(it->path,&st)<0||!(st.st_mode&FIO_S_IFREG)||st.st_size<=0)continue;
            snprintf(it->name,sizeof(it->name),"%s",de.d_name);snprintf(it->artist,sizeof(it->artist),"本地视频");
            it->size=st.st_size>0x7fffffff?0x7fffffff:(int)st.st_size;it->state=2;it->pct=100;
            if(++local_video_count==16)break;
        }sceIoDclose(d);
    }
}

int mv_dl_snapshot(MvDlItem *out, int max)
{
    unsigned now = (unsigned)sceKernelGetSystemTimeLow();

    if (!out || max <= 0) return 0;
    if (max > MV_SNAP_MAX) max = MV_SNAP_MAX;
    {
        int st = api_mv_download_state();

        if (st != g_snap_state) {
            g_snap_state = st;
            g_snap_last = 0;
        }
    }
    /* 1.5 秒的缓存：UI 每帧都会调，索引只在下载完成/删除/起播时变。 */
    if (g_snap_last && now - g_snap_last < 1500000) {
        int n = g_snap_n < max ? g_snap_n : max;

        memcpy(out, g_snap, n * sizeof(MvDlItem));
        /* The 1.5s cache must not freeze the live row's numbers: progress and
         * size are cheap reads and the list is where the user watches them. */
        if (n > 0 && out[0].state == 1) {
            out[0].mvid = api_mv_info()->id;
            out[0].pct = api_mv_download_pct();
            out[0].size = (int)api_mv_download_total();
        }
        return n;
    }
    g_snap_last = now;
    g_snap_n = 0;

    /* 正在下载的排最前 */
    if (api_mv_download_state() == 1) {
        const MvInfo *mi = api_mv_info();
        MvDlItem *it = &g_snap[g_snap_n++];

        memset(it, 0, sizeof(*it));
        it->mvid = mi->id;
        snprintf(it->name, sizeof(it->name), "%s", mi->name[0] ? mi->name : "MV");
        snprintf(it->artist, sizeof(it->artist), "%s", mi->artist);
        snprintf(it->path, sizeof(it->path), MV_CACHE "/mv_%d_240.mp4", mi->id);
        it->size = (int)api_mv_download_total();
        it->state = 1;
        it->pct = api_mv_download_pct();
    }
    mv_scan_local_videos();
    for(int i=0;i<local_video_count&&g_snap_n<max;++i)g_snap[g_snap_n++]=local_videos[i];
    if (g_snap_n < max)
        g_snap_n += mv_index_load(&g_snap[g_snap_n], max - g_snap_n);

    {
        int n = g_snap_n < max ? g_snap_n : max;

        memcpy(out, g_snap, n * sizeof(MvDlItem));
        return n;
    }
}

int mv_dl_delete(int idx)
{
    MvDlItem items[MV_SNAP_MAX];
    int n;

    g_snap_last = 0;
    n = mv_dl_snapshot(items, MV_SNAP_MAX);
    if (idx < 0 || idx >= n) return -1;
    if (items[idx].state == 1) {
        api_mv_download_abort();
        return 0;
    }
    if(items[idx].mvid<=0)return -2; /* Local originals are not download-cache entries. */
    if(sceIoRemove(items[idx].path)<0)return -1;
    {
        char meta[160];

        snprintf(meta, sizeof(meta), MV_CACHE "/mv_%d_240.txt", items[idx].mvid);
        sceIoRemove(meta);
    }
    mv_index_remove(items[idx].mvid);
    g_snap_last = 0;
    return 0;
}
