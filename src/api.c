/*
 * MELODY PSP — 网易云接口 + 后台请求线程
 */

#include "api.h"
#include <unistd.h>
#include "update_gallery.h"
#include "mv.h"
#include "store.h"
#include "community.h"
#include "alt.h"
#include "jpegdec.h"
#include "coverjpeg.h"
#include "art_fit.h"
#include "png.h"
#include "local.h"

#include "audio.h"
#include "cache.h"
#include "skin_catalog.h"
#include "dl.h"
#include "gfx.h"
#include "json.h"
#include "qr.h"
#include "log.h"
#include "net.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspjpeg.h>
#include <pspnet_apctl.h>
#include <pspthreadman.h>
#include <psputility.h>
#include <psputility_avmodules.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session_store.h"
#include "update_pbp.h"

#define BUF_SIZE      (320 * 1024)
#define SEARCH_PAGE   50
#define SUGGEST_MAX   10
#define JPEG_MAX      (72 * 1024)
/* 封面解码：192 足够（播放页 134px、列表 48px、背景 480 但只是放大），
 * 256 会多占 173KB —— MV 回来时就是这两块把 2MB 堆挤爆的。 */
#define ART_MAX       192
#define WORKER_STACK  0x18000
#define WORKER_PRIO   0x21

enum {
    JOB_NONE = 0,
    JOB_PROBE,
    JOB_CONNECT,
    JOB_CHART,
    JOB_SEARCH,
    JOB_SKIN_REFRESH,
    JOB_SKIN_INSTALL,
    JOB_SKIN_PREVIEW,
    JOB_SEARCH_CATEGORY,
    JOB_SEARCH_OPEN,
    JOB_SUGGEST,
    JOB_QR_NEW,
    JOB_QR_POLL,
    JOB_ACCOUNT,
    JOB_DAILY,
    JOB_REC_LISTS,
    JOB_MY_LISTS,
    JOB_TOPLISTS,
    JOB_ARTIST_TOP,
    JOB_CAT_LISTS,
    JOB_PLAYLIST,
    JOB_PLAYLIST_MORE,
    JOB_LIKED,
    JOB_LIKED_TOTAL,
    JOB_PLAYED,
    JOB_ARTIST_SONGS,
    JOB_COMMENTS,
    JOB_COMMENT_REPLIES,
    JOB_COMMENT_COUNT,
    JOB_USER_DETAIL,
    JOB_THUMBS,
    JOB_CHECK,
    JOB_PLAY,
    JOB_LYRIC,
    JOB_EXPORT,
    JOB_ART,
    JOB_LOCAL_META,
    JOB_LOCAL_ID,
    JOB_DOWNLOAD,
    JOB_LIKE,
    JOB_SUBSCRIBE,
    JOB_UPDATE_CHECK,
    JOB_UPDATE_DL,
    JOB_MV_DETAIL,
    JOB_MV_DL,
    JOB_LIKES_REFRESH,
    JOB_SAVED_PLAYLISTS,
    JOB_ANNOUNCEMENT_CHECK,
    JOB_ANNOUNCEMENT_DISMISS,
    JOB_DAILY_PREVIEW,
};

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

static char *g_buf;
/* Result workspace lives at the high end of user memory, outside the small
 * libc heap. It is retired with API buffers before MV to restore a continuous
 * decoder region; no permanent second SongList remains in the PRX BSS. */
static Song g_daily_preview;
static unsigned g_daily_preview_account;
static unsigned g_daily_preview_request_account, g_daily_preview_retry_at;
static int g_daily_preview_pending, g_daily_preview_inflight, g_daily_preview_attempted;
static SongList*g_pending_storage;
static SceUID g_pending_block=-1;
#define g_pending (*g_pending_storage)

/* 搜索分页 / 联想 */
static char g_search_q[64];
static int g_search_off;          /* 已经拿到多少首（下次 offset） */
static int g_search_total;        /* 服务端命中总数 */
static int g_list_total_hint;     /* 当前列表在服务端的完整条数（歌单/搜索/歌手…） */
static volatile int g_search_append;   /* 本次 JOB_SEARCH 是"下一页" */
static char g_sug[SUGGEST_MAX][64];
static int g_sug_n;
static Lyric g_lyric;

static unsigned short *g_art;
static int g_art_w, g_art_h;
static unsigned short *g_backdrop;
static unsigned short *g_backdrop_dim;
static SceUID g_backdrop_block=-1;
static unsigned char *g_jpg;
static unsigned char *g_rgba;

static int g_lock = -1;
static int g_thid = -1;
static volatile int g_skin_request_busy;
static volatile int g_quit;
static volatile int g_busy;
static int g_art_cpu_only; /* Sticky after MV takes ME ownership. */
static int g_jpeg_on;         /* sceJpegInitMJpeg 成功过才需要 Finish */

/* 任务队列：播放请求单独一条（要抢在别的任务前面），其余 FIFO */
#define JOBQ_MAX 20
typedef struct {
    int job;
    int arg;
    char query[128];
    Song song;
} JobItem;

static volatile int g_abort;      /* 播放请求来了：把正在跑的小请求打断 */
static JobItem g_play;            /* job==0 表示没有 */
static JobItem g_jq[JOBQ_MAX];
static int g_jq_head, g_jq_tail, g_jq_n;
static Song g_download_requests[32];
static unsigned char g_download_silent[32];
static int g_download_request_n,g_download_job_silent;
static unsigned g_download_epoch,g_download_job_epoch;

static ApiEvent g_ev[24];
static int g_ev_head, g_ev_tail;

static Chart g_charts[] = {
    { 3778678,  "热歌榜", "TOP 20", 0, 0, 0 },
    { 3779629,  "新歌榜", "TOP 20", 0, 0, 1 },
    { 19723756, "飙升榜", "TOP 20", 0, 0, 2 }
};

/* PSP 的 SDK 只有信号量，没有 mutex；用二值信号量当锁 */
static int lock_new(const char *name)
{
    return sceKernelCreateSema(name, 0, 1, 1, NULL);
}

/* 带超时的二值信号量锁。为什么要超时：api_shutdown() 3 秒等不到 api 线程就
 * sceKernelTerminateDeleteThread 强杀它，如果它此刻正握着这把锁，锁永远回不来
 * —— 主线程下一句 api_clear_events()/queue_job() 就会永久卡死（PPSSPP 上
 * 「菜单停在播放 MV、界面冻住」的机制）。拿不到锁时：
 *   - 记一次超时，跳过这次临界区（事件丢一个，比整机卡死好）；
 *   - UNLOCK 会认自己没拿到，绝不放掉别人的锁。
 * 正常路径（没人被强杀）行为和以前一样。 */
#define LOCK_TIMEOUT_US  (100 * 1000)
#define LOCK_HOLD_MAX    4

/* 谁现在拿着锁（线程 id + 锁 id）。只有持有者能 unlock —— 超时没拿到的
 * 线程 signal 会放掉别人的锁；而为了避免「脏标记」，这里只记录持有者，
 * 拿不到就什么都不记（第一版用“跳过表”反而把自己下一次的锁锁死了）。 */
static SceUID g_hold_thid[LOCK_HOLD_MAX] = { -1, -1, -1, -1 };
static SceUID g_hold_id[LOCK_HOLD_MAX] = { -1, -1, -1, -1 };
static volatile int g_lock_timeouts;

static int hold_add(SceUID me, SceUID id)
{
    int i;

    for (i = 0; i < LOCK_HOLD_MAX; ++i) {
        if (g_hold_thid[i] == me && g_hold_id[i] == id) return 1;
        if (g_hold_thid[i] < 0) {
            g_hold_thid[i] = me;
            g_hold_id[i] = id;
            return 1;
        }
    }
    return 0;
}

static int hold_del(SceUID me, SceUID id)
{
    int i;

    for (i = 0; i < LOCK_HOLD_MAX; ++i) {
        if (g_hold_thid[i] == me && g_hold_id[i] == id) {
            g_hold_thid[i] = -1;
            g_hold_id[i] = -1;
            return 1;
        }
    }
    return 0;
}

static int lock_take(int id)
{
    SceUInt tmo = LOCK_TIMEOUT_US;

    if (id < 0) return 0;
    if (sceKernelWaitSema(id, 1, &tmo) < 0) {
        ++g_lock_timeouts;
        return 0;
    }
    hold_add(sceKernelGetThreadId(), id);
    return 1;
}

static void lock_give(int id)
{
    SceUID me = sceKernelGetThreadId();

    if (id < 0) return;
    if (!hold_del(me, id)) return;        /* 没拿到就别放别人的锁 */
    sceKernelSignalSema(id, 1);
}

/* 强杀过线程之后调用：旧锁可能被带走，换一把新的。
 * 还在等旧锁的线程会直接拿到错误码，不会卡死。 */
void api_reset_locks(void)
{
    int old = g_lock;
    int i;

    for (i = 0; i < LOCK_HOLD_MAX; ++i) {
        g_hold_thid[i] = -1;
        g_hold_id[i] = -1;
    }
    g_lock = lock_new("melody_api2");
    if (old >= 0) sceKernelDeleteSema(old);
    mlog("api: locks recreated after forced kill (timeouts=%d)", g_lock_timeouts);
}

int api_lock_timeouts(void) { return g_lock_timeouts; }

#define LOCK(id, ...)   lock_take(id)
#define UNLOCK(id, ...) lock_give(id)

/* ------------------------------------------------------------------ */
/* 事件队列                                                            */
/* ------------------------------------------------------------------ */

static void post_ex(int kind, int index, long long subject_id,
                    unsigned int account_generation, unsigned int request_generation,
                    const char *msg)
{
    if (!lock_take(g_lock)) return;   /* 锁被强杀线程带走时丢一个事件，不死等 */

    {
        int next = (g_ev_tail + 1) % (int)(sizeof(g_ev) / sizeof(g_ev[0]));
        if (next != g_ev_head) {
            ApiEvent *e = &g_ev[g_ev_tail];
            e->kind = kind;
            e->index = index;
            e->subject_id = subject_id;
            e->account_generation = account_generation;
            e->request_generation = request_generation;
            snprintf(e->msg, sizeof(e->msg), "%s", msg ? msg : "");
            g_ev_tail = next;
            mlog("event %d idx=%d %s", kind, index, e->msg);
        }
    }

    UNLOCK(g_lock, 1);
}

static void post(int kind, int index, const char *msg)
{
    post_ex(kind, index, 0, 0, 0, msg);
}

int api_poll(ApiEvent *ev)
{
    int got = 0;

    if (!lock_take(g_lock)) return 0;   /* 超时就当这帧没有事件，下帧再看 */
    if (g_ev_head != g_ev_tail) {
        *ev = g_ev[g_ev_head];
        g_ev_head = (g_ev_head + 1) % (int)(sizeof(g_ev) / sizeof(g_ev[0]));
        got = 1;
    }
    lock_give(g_lock);
    return got;
}

int api_busy(void) { return g_busy; }

/* MV 开始前把还没处理完的事件丢掉：api 线程停下后队列里可能还压着
 * EV_LIST_OK / EV_SONGS_OK，主线程一处理就会把界面从 MV 切回列表。 */
void api_clear_events(void)
{
    if (!lock_take(g_lock)) return;     /* 拿不到就别在这里陪葬，MV 起播优先 */
    g_ev_head = g_ev_tail = 0;
    lock_give(g_lock);
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static int seg_is(const char *p, int len, const char *lit)
{
    int n = (int)strlen(lit);
    return len == n && !strncmp(p, lit, (size_t)n);
}

static void safe_copy(char *dst, int dstlen, const char *src)
{
    if (dstlen <= 0) return;
    snprintf(dst, (size_t)dstlen, "%s", src ? src : "");
}

static int fetch_once(const char *url, char *err, int errlen, int *len)
{
    *len = 0;
    /* 修复：api_init 分配失败时 g_buf == NULL，所有请求直接失败，别写空指针 */
    if (!g_buf) {
        if (err && errlen > 0) snprintf(err, (size_t)errlen, "%s", "内存不足");
        return -1;
    }
    http_set_abort(&g_abort);
    if (http_get(url, g_buf, BUF_SIZE, len, 9000, err, errlen) < 0) return -1;
    g_buf[*len] = 0;
    return 0;
}

static int fetch(const char *url, char *err, int errlen)
{
    int len = 0;
    int expect;

    if (fetch_once(url, err, errlen, &len) < 0) {
        mlog("fetch fail: %s -> %s (abort=%d)", url, err, (int)g_abort);

        /* WLAN 掉线了？连回去再试一次 */
        if (!g_abort && net_reconnect() == 0) {
            sceKernelDelayThread(120 * 1000);
            if (fetch_once(url, err, errlen, &len) >= 0) {
                expect = http_get_expect();
                if (expect <= 0 || len >= expect - 64) return 0;
            }
        }
        return -1;
    }

    expect = http_get_expect();

    /* 正文比 Content-Length 短 = 连接被掐断了，重连一次再拿一遍 */
    if (expect > 0 && len < expect - 64 && !g_abort) {
        mlog("fetch short %d/%d, retry", len, expect);
        sceKernelDelayThread(80 * 1000);
        if (fetch_once(url, err, errlen, &len) < 0) {
            mlog("fetch retry fail: %s -> %s", url, err);
            return -1;
        }
    }

    g_buf[len] = 0;
    mlog("fetched %d bytes", len);
    return len;
}

/* ------------------------------------------------------------------ */
/* 列表解析（榜单 / 搜索同构：ar[].name + al.picUrl + dt + fee）        */
/* ------------------------------------------------------------------ */

typedef struct {
    SongList *list;
    int last_idx;
    int base;
} ListCtx;

static void list_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    ListCtx *ctx = (ListCtx *)ud;
    const char *seg[10];
    int len[10];
    int n = json_split_path(path, seg, len, 10);
    int a = -1;

    if (n <= 0) return;
    if (type == JSON_STR && n == 2 && seg_is(seg[0], len[0], "playlist") &&
        seg_is(seg[1], len[1], "name")) {
        safe_copy(ctx->list->title, sizeof(ctx->list->title), str);
        return;
    }

    for (int i = 0; i < n; ++i) {
        if (seg_is(seg[i], len[i], "tracks") || seg_is(seg[i], len[i], "songs") ||
            seg_is(seg[i], len[i], "dailySongs"))
            a = i;
    }
    if (a < 0 || a + 2 >= n) return;

    {
        int idx = atoi(seg[a + 1]) + ctx->base;
        Song *s;

        if (idx < 0 || idx >= MAX_SONGS) return;
        s = &ctx->list->songs[idx];
        if (idx + 1 > ctx->list->count) ctx->list->count = idx + 1;
        if (idx != ctx->last_idx) {
            ctx->last_idx = idx;
            memset(s, 0, sizeof(*s));
        }

        /* ...songs.N.<field> */
        if (n == a + 3) {
            const char *f = seg[a + 2];
            int fl = len[a + 2];

            if (seg_is(f, fl, "id")) s->id = num;
            else if (seg_is(f, fl, "name")) safe_copy(s->name, sizeof(s->name), str);
            else if (seg_is(f, fl, "dt")) s->dur_ms = (int)num;
            else if (seg_is(f, fl, "duration")) s->dur_ms = (int)num;
            else if (seg_is(f, fl, "fee")) s->fee = (int)num;
            else if (seg_is(f, fl, "pop")) s->pop = (int)num;
            else if (seg_is(f, fl, "mv") || seg_is(f, fl, "mvid")) s->mv = (int)num;
            return;
        }

        /* ...songs.N.al.picUrl / al.name */
        if (n == a + 4 && (seg_is(seg[a + 2], len[a + 2], "al") ||
                           seg_is(seg[a + 2], len[a + 2], "album"))) {
            const char *f = seg[a + 3];
            int fl = len[a + 3];

            if (seg_is(f, fl, "picUrl")) safe_copy(s->art, sizeof(s->art), str);
            else if (seg_is(f, fl, "name")) safe_copy(s->album, sizeof(s->album), str);
            return;
        }

        /* ...songs.N.ar.M.name （M=0 直接写，其余用 / 追加） */
        if (n == a + 5 && (seg_is(seg[a + 2], len[a + 2], "ar") ||
                           seg_is(seg[a + 2], len[a + 2], "artists"))) {
            int ai = atoi(seg[a + 3]);

            if (seg_is(seg[a + 4], len[a + 4], "name") && str && str[0]) {
                int cur = (int)strlen(s->artist);
                if (ai == 0 || cur == 0) {
                    safe_copy(s->artist, sizeof(s->artist), str);
                } else if (cur + 1 + (int)strlen(str) < (int)sizeof(s->artist)) {
                    s->artist[cur] = '/';
                    safe_copy(s->artist + cur + 1, (int)sizeof(s->artist) - cur - 1, str);
                }
            }
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 播放地址解析                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char url[900];
    int size;
    int br;
    int code;
    int found;
    int trial;          /* 1 = 只有 45 秒试听（freeTrialInfo 非空） */
} UrlCtx;

static void url_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    UrlCtx *ctx = (UrlCtx *)ud;
    const char *seg[6];
    int len[6];
    int n = json_split_path(path, seg, len, 6);

    /* data.N.freeTrialInfo.end（v1 接口给试听区间；存在就说明是试听） */
    if (n == 4 && seg_is(seg[0], len[0], "data") &&
        seg_is(seg[2], len[2], "freeTrialInfo")) {
        if (atoi(seg[1]) == 0) ctx->trial = 1;
        return;
    }
    if (n != 3 || !seg_is(seg[0], len[0], "data")) return;

    if (type == JSON_NUM && seg_is(seg[2], len[2], "code")) {
        if (atoi(seg[1]) == 0) ctx->code = (int)num;
    } else if (type == JSON_NUM && seg_is(seg[2], len[2], "size")) {
        if (atoi(seg[1]) == 0) ctx->size = (int)num;
    } else if (type == JSON_NUM && seg_is(seg[2], len[2], "br")) {
        if (atoi(seg[1]) == 0) ctx->br = (int)num;
    } else if (type == JSON_STR && seg_is(seg[2], len[2], "url")) {
        if (atoi(seg[1]) == 0 && str && str[0]) {
            safe_copy(ctx->url, sizeof(ctx->url), str);
            ctx->found = 1;
        }
    }
}

static int url_walk_ok(UrlCtx *ctx)
{
    if (!ctx->found) return 0;
    if (strncasecmp(ctx->url, "http://", 7) != 0) return 0;
    if (ctx->size <= 0) ctx->size = 0;
    return 1;
}

/* 老接口：免费歌/低音质直接从 CDN 给直链，响应小，匿名可取。
 * 返回 1 = 拿到完整曲，0 = 没有。 */
static int resolve_url_legacy(const Song *song, UrlCtx *ctx, char *err, int errlen)
{
    char url[512];
    int br = api_quality_br();

    memset(ctx, 0, sizeof(*ctx));
    snprintf(url, sizeof(url),
             API_BASE "/api/song/enhance/player/url?id=%lld&ids=%%5B%lld%%5D&br=%d",
             song->id, song->id, br);
    if (fetch(url, err, errlen) < 0) return 0;
    json_walk(g_buf, (int)strlen(g_buf), url_cb, ctx);

    if ((!ctx->found || ctx->size <= 0) && br != 128000) {
        /* 高音质没货，退回标准音质再试一次 */
        snprintf(url, sizeof(url),
                 API_BASE "/api/song/enhance/player/url?id=%lld&ids=%%5B%lld%%5D&br=128000",
                 song->id, song->id);
        memset(ctx, 0, sizeof(*ctx));
        if (fetch(url, err, errlen) >= 0)
            json_walk(g_buf, (int)strlen(g_buf), url_cb, ctx);
    }
    return url_walk_ok(ctx);
}

/* v1 接口：带 os=pc/appver 才能拿到地址；VIP 歌返回 45 秒试听，
 * 靠 freeTrialInfo 判断（size/time 也能看出来，但字段更直接）。 */
static int resolve_url_v1(const Song *song, UrlCtx *ctx, char *err, int errlen)
{
    char url[420];
    const char *level = "standard";

    if (g_app.quality == 2) level = "exhigh";
    else if (g_app.quality == 1) level = "higher";

    memset(ctx, 0, sizeof(*ctx));
    snprintf(url, sizeof(url),
             API_BASE "/api/song/enhance/player/url/v1?ids=%%5B%lld%%5D&level=%s&encodeType=mp3",
             song->id, level);
    if (fetch(url, err, errlen) < 0) return 0;
    json_walk(g_buf, (int)strlen(g_buf), url_cb, ctx);
    return url_walk_ok(ctx);
}

/* 取直链：先试老接口（匿名、小响应），不行再试 v1（能拿 45 秒试听）。
 * 返回 1 = 有 URL（*ctx->trial 标记是否只有试听），0 = 完全没有。 */
static int resolve_url(const Song *song, UrlCtx *ctx, char *err, int errlen)
{
    /* 先用匿名身份取地址：带 cookie 拿到的是会话签名 URL，CDN 会 403 */
    net_skip_cookie(1);
    if (resolve_url_legacy(song, ctx, err, errlen)) {
        net_skip_cookie(0);
        return 1;
    }
    if (resolve_url_v1(song, ctx, err, errlen)) {
        net_skip_cookie(0);
        return 1;
    }
    net_skip_cookie(0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 封面                                                                */
/* ------------------------------------------------------------------ */

static void rgba_to_565(const unsigned char *rgba, int w, int h, unsigned short *out)
{
    for (int y = 0; y < h; ++y) {
        const unsigned char *p = rgba + (size_t)y * w * 4;
        unsigned short *row = out + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            row[x] = (unsigned short)(((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3));
            p += 4;
        }
    }
}

/* 把封面放大 + 压暗，做成播放页背景（只做一次） */
static void build_backdrop(const unsigned short *art, int aw, int ah)
{
    /* 修复：api_init 分配失败时 g_backdrop 可能是 NULL，直接返回 */
    if (!g_backdrop) return;

    for (int y = 0; y < SCR_H; ++y) {
        unsigned short *row = g_backdrop + (size_t)y * SCR_W;
        int sy = y * ah / SCR_H;
        int vy = y - SCR_H / 2;
        int vig = 256 - (vy * vy * 90) / ((SCR_H / 2) * (SCR_H / 2));

        for (int x = 0; x < SCR_W; ++x) {
            int sx = x * aw / SCR_W;
            unsigned short c = art[(size_t)sy * aw + sx];
            int r = (c & 0x1F) * 5 / 12;
            int g = ((c >> 5) & 0x3F) * 5 / 12;
            int b = ((c >> 11) & 0x1F) * 5 / 12;
            int vx = x - SCR_W / 2;
            int v = vig - (vx * vx * 40) / ((SCR_W / 2) * (SCR_W / 2));

            if (v < 170) v = 170;
            r = r * v / 256;
            g = g * v / 256;
            b = b * v / 256;
            row[x] = (unsigned short)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
        }
    }

    if (g_backdrop_dim) {
        for (int y = 0; y < SCR_H; ++y) {
            const unsigned short *src = g_backdrop + (size_t)y * SCR_W;
            unsigned short *dst = g_backdrop_dim + (size_t)y * SCR_W;

            for (int x = 0; x < SCR_W; ++x)
                dst[x] = gfx_lerp565(src[x], gfx_rgb(0, 0, 0), 140);
        }
    }
}

static int decode_cover_rgba(const unsigned char *bytes, int len, int maxdim, int *w, int *h);

static int decode_art_png(const unsigned char *png, int len)
{
    int w = 0, h = 0;

    if (len < 33 || memcmp(png + 12, "IHDR", 4)) {
        mlog("art: png bad header (%d bytes)", len);
        return -1;
    }

    /* 直接解成 256 以内，不再需要 1.6MB 的满尺寸临时缓冲 */
    if (decode_cover_rgba(png, len, ART_MAX, &w, &h) < 0) {
        int pw = (int)(((unsigned)png[16] << 24) | ((unsigned)png[17] << 16) |
                       ((unsigned)png[18] << 8) | png[19]);
        int ph = (int)(((unsigned)png[20] << 24) | ((unsigned)png[21] << 16) |
                       ((unsigned)png[22] << 8) | png[23]);

        mlog("art: png decode failed %dx%d (%d bytes)", pw, ph, len);
        return -1;
    }

    rgba_to_565(g_rgba, w, h, g_art);
    g_art_w = w;
    g_art_h = h;
    build_backdrop(g_art, w, h);
    mlog("art: png ok %dx%d", w, h);
    return 0;
}

static int decode_art(const unsigned char *jpg, int len);

/* UTF-16LE -> UTF-8（ID3 文本帧用） */
static void utf16le_to_utf8(const unsigned char *in, int bytes, char *out, int max)
{
    int o = 0;

    for (int i = 0; i + 1 < bytes && o + 4 < max; i += 2) {
        unsigned int cp = in[i] | ((unsigned int)in[i + 1] << 8);

        if (cp == 0) break;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < bytes) {
            unsigned int lo = in[i + 2] | ((unsigned int)in[i + 3] << 8);

            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                i += 2;
            }
        }
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
}

/* 把 ID3 文本帧（ISO-8859-1 / UTF-16）转成 UTF-8 */
static void id3_text_to_utf8(const unsigned char *d, int len, char *out, int max)
{
    int enc;

    out[0] = 0;
    if (len < 2) return;
    enc = d[0];
    d++;
    len--;

    if (enc == 1 || enc == 2) {
        if (len >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
            utf16le_to_utf8(d + 2, len - 2, out, max);
        } else if (len >= 2 && d[0] == 0xFE && d[1] == 0xFF) {
            /* UTF-16BE：就地翻转 */
            static unsigned char tmp[256];
            int n = len - 2 < (int)sizeof(tmp) ? len - 2 : (int)sizeof(tmp);

            for (int i = 0; i + 1 < n; i += 2) {
                tmp[i] = d[i + 3];
                tmp[i + 1] = d[i + 2];
            }
            utf16le_to_utf8(tmp, n, out, max);
        } else {
            utf16le_to_utf8(d, len, out, max);
        }
    } else {
        /* latin1 -> utf8 */
        int o = 0;

        for (int i = 0; i < len && o + 3 < max; ++i) {
            unsigned int c = d[i];

            if (c == 0) break;
            if (c < 0x80) out[o++] = (char)c;
            else {
                out[o++] = (char)(0xC0 | (c >> 6));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
        out[o] = 0;
    }
}

/* 读本地 mp3 的 ID3 标题/歌手到 Song（文件名是 GBK，不能直接显示） */
/* ------------------------------------------------------------------ */
/* 本地 mp3 的 ID3（流式：不把整个标签读进内存）                        */
/* ------------------------------------------------------------------ */

static int id3_tag_len(FILE *fp, int *ver)
{
    unsigned char hdr[10];
    int len;

    if (fread(hdr, 1, 10, fp) != 10 || memcmp(hdr, "ID3", 3)) return -1;
    *ver = hdr[3];
    len = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
          ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
    return len > 0 ? len : -1;
}

/* 取下一个帧头；成功返回帧长度（文件指针已移到帧数据开头）。
 * 注意：不能像以前那样「标签超过 512KB 就放弃」——带大封面的歌标签就是会超。 */
static int id3_next_frame(FILE *fp, int ver, int *pos, int taglen, char id[5])
{
    while (*pos + 10 <= taglen) {
        unsigned char fh[10];
        int fsz;

        if (fseek(fp, 10 + *pos, SEEK_SET) != 0) return -1;
        if (fread(fh, 1, 10, fp) != 10) return -1;

        if (ver >= 4)
            fsz = ((fh[4] & 0x7F) << 21) | ((fh[5] & 0x7F) << 14) |
                  ((fh[6] & 0x7F) << 7) | (fh[7] & 0x7F);
        else
            fsz = ((int)fh[4] << 24) | ((int)fh[5] << 16) | ((int)fh[6] << 8) | fh[7];

        if (fsz <= 0 || *pos + 10 + fsz > taglen) return -1;
        memcpy(id, fh, 4);
        id[4] = 0;
        *pos += 10 + fsz;
        return fsz;
    }
    return -1;
}

/* 读本地 mp3 的 ID3 标题/歌手到 Song（文件名是 GBK，不能直接显示） */
int api_local_meta(const char *path, Song *song)
{
    FILE *fp;
    int taglen, ver, pos = 0, got = 0;
    char id[5];

    if (!path || !song) return 0;
    fp = fopen(path, "rb");
    if (!fp) return 0;
    taglen = id3_tag_len(fp, &ver);
    if (taglen <= 0) {
        fclose(fp);
        return 0;
    }

    while (pos + 10 <= taglen) {
        int fsz = id3_next_frame(fp, ver, &pos, taglen, id);
        unsigned char *buf;

        if (fsz <= 0) break;
        if (memcmp(id, "TIT2", 4) && memcmp(id, "TPE1", 4) && memcmp(id, "TALB", 4)) continue;
        if (fsz > 4096) continue;

        buf = (unsigned char *)malloc((size_t)fsz);
        if (!buf) break;
        if ((int)fread(buf, 1, (size_t)fsz, fp) != fsz) {
            free(buf);
            break;
        }
        if (!memcmp(id, "TIT2", 4)) {
            id3_text_to_utf8(buf, fsz, song->name, sizeof(song->name));
            got = 1;
        } else if (!memcmp(id, "TPE1", 4)) {
            id3_text_to_utf8(buf, fsz, song->artist, sizeof(song->artist));
            got = 1;
        } else {
            id3_text_to_utf8(buf, fsz, song->album, sizeof(song->album));
        }
        free(buf);
    }

    fclose(fp);
    return got;
}

/* ------------------------------------------------------------------ */
/* 封面小缓存：解一次，存成 565 小图，下次直接读                        */
/* ------------------------------------------------------------------ */

#define ART_CACHE_DIR   "./art"

static void art_cache_ready(void)
{
    static int done;

    if (done) return;
    done = 1;
    sceIoMkdir(ART_CACHE_DIR, 0777);
}

static unsigned int art_key_for(const char *path)
{
    FILE *f;
    long n = 0;
    unsigned int h = 2166136261u;

    if (!path) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);

    for (const char *p = path; *p; ++p) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    h ^= (unsigned int)n;
    h *= 16777619u;
    return h ? h : 1;
}

static int art_cache_load(unsigned int key)
{
    char p[96];
    unsigned char hdr[8];
    FILE *f;
    int w = 0, h = 0, n = 0;
    int ok = 0;

    if (!key || !g_art) return -1;      /* 修复：内存不足时别往空指针里读 */
    snprintf(p, sizeof(p), ART_CACHE_DIR "/%08X.bin", key);

    /* 修复：封面缓存文件的读整段放进全局 stdio 锁（mlog 留在锁外） */
    io_lock();
    f = fopen(p, "rb");
    if (f) {
        if (fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, "ART1", 4)) {
            w = hdr[4] | (hdr[5] << 8);
            h = hdr[6] | (hdr[7] << 8);
            if (w > 0 && h > 0 && w <= ART_MAX && h <= ART_MAX) {
                n = w * h;
                if ((int)fread(g_art, 2, (size_t)n, f) == n) ok = 1;
            }
        }
        fclose(f);
    }
    io_unlock();

    if (!ok) return -1;
    g_art_w = w;
    g_art_h = h;
    build_backdrop(g_art, w, h);
    mlog("local art: cache hit %08X %dx%d", key, w, h);
    return 0;
}

static void art_cache_save(unsigned int key)
{
    char p[96];
    unsigned char hdr[8];
    FILE *f;

    if (!key || !g_art || g_art_w <= 0 || g_art_h <= 0) return;
    art_cache_ready();
    snprintf(p, sizeof(p), ART_CACHE_DIR "/%08X.bin", key);

    /* 修复：写封面缓存整段放进全局 stdio 锁（mlog 会再取锁，必须放外面） */
    io_lock();
    f = fopen(p, "wb");
    if (f) {
        memcpy(hdr, "ART1", 4);
        hdr[4] = (unsigned char)(g_art_w & 0xFF);
        hdr[5] = (unsigned char)(g_art_w >> 8);
        hdr[6] = (unsigned char)(g_art_h & 0xFF);
        hdr[7] = (unsigned char)(g_art_h >> 8);
        fwrite(hdr, 1, 8, f);
        fwrite(g_art, 2, (size_t)(g_art_w * g_art_h), f);
        fclose(f);
    }
    io_unlock();
    mlog("local art: cached %08X %dx%d", key, g_art_w, g_art_h);
}
/* 从本地 mp3 的 ID3 里抠出内嵌封面并解码（本地音乐页用） */
int api_local_art(const char *path)
{
    FILE *fp;
    int taglen, ver, pos = 0;
    char id[5];
    unsigned int key;

    if (!path) return -1;

    key = art_key_for(path);
    if (art_cache_load(key) == 0) return 0;

    fp = fopen(path, "rb");
    if (!fp) return -1;
    taglen = id3_tag_len(fp, &ver);
    if (taglen <= 0) {
        fclose(fp);
        return -1;
    }

    while (pos + 10 <= taglen) {
        int fsz = id3_next_frame(fp, ver, &pos, taglen, id);
        unsigned char *buf;
        const unsigned char *d, *end;
        int n, r, enc;

        if (fsz <= 0) break;
        if (memcmp(id, "APIC", 4)) continue;

        /* 只读这一帧（封面本身），4MB 上限防呆 */
        if (fsz > 4 * 1024 * 1024) {
            mlog("local art: APIC too big %d", fsz);
            break;
        }
        buf = (unsigned char *)malloc((size_t)fsz);
        if (!buf) {
            mlog("local art: no mem for %d bytes", fsz);
            break;
        }
        if ((int)fread(buf, 1, (size_t)fsz, fp) != fsz) {
            free(buf);
            break;
        }

        d = buf;
        end = buf + fsz;
        enc = *d++;
        {
            const unsigned char *nul = (const unsigned char *)memchr(d, 0, (size_t)(end - d));

            if (!nul) { free(buf); break; }
            d = nul + 1;              /* MIME */
        }
        d++;                          /* 图片类型 */
        if (enc == 1 || enc == 2) {
            while (d + 1 < end && !(d[0] == 0 && d[1] == 0)) d += 2;
            d += 2;
        } else {
            const unsigned char *nul = (const unsigned char *)memchr(d, 0, (size_t)(end - d));

            if (!nul) { free(buf); break; }
            d = nul + 1;
        }

        if (end - d > 16) {
            n = (int)(end - d);
            r = decode_art(d, n);
            free(buf);
            mlog("local art: APIC %d bytes -> %d", n, r);
            fclose(fp);
            if (r == 0) art_cache_save(key);
            return r;
        }
        free(buf);
        break;
    }

    fclose(fp);
    return -1;
}
/* All album-art CPU decoding uses the existing bounded RGBA workspace. */
static int decode_cover_rgba(const unsigned char *bytes, int len, int maxdim, int *w, int *h)
{
    int sw = 0, sh = 0, progressive = 0, rc;
    if (!g_rgba || !bytes || len < 8 || maxdim <= 0 || maxdim > ART_MAX) return -1;
    if (!memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) {
        unsigned pw, ph;
        if (len < 33 || memcmp(bytes + 12, "IHDR", 4)) return -1;
        pw = ((unsigned)bytes[16] << 24) | ((unsigned)bytes[17] << 16) |
             ((unsigned)bytes[18] << 8) | bytes[19];
        ph = ((unsigned)bytes[20] << 24) | ((unsigned)bytes[21] << 16) |
             ((unsigned)bytes[22] << 8) | bytes[23];
        if (!pw || !ph || pw > 1024 || ph > 1024 || (size_t)pw * ph > 1024u * 1024u) return -1;
        return png_decode_rgba_scaled(bytes, len, g_rgba, maxdim, w, h);
    }
    if (jpeg_read_size(bytes, len, &sw, &sh, &progressive) < 0 ||
        sw <= 0 || sh <= 0 || sw > COVERJPEG_MAX_SOURCE_DIM || sh > COVERJPEG_MAX_SOURCE_DIM ||
        bytes[len - 2] != 0xFF || bytes[len - 1] != 0xD9) return -1;
    if (!progressive) {
        rc = jpeg_decode_rgba_scaled(bytes, len, g_rgba, maxdim, w, h);
        if (rc == 0) return 0;
    }
    return coverjpeg_decode_rgba_scaled(bytes, len, g_rgba,
                                       ART_MAX * ART_MAX * 4u, maxdim, w, h);
}

static int decode_art(const unsigned char *jpg, int len)
{
    int w = 0, h = 0, progressive = 0;
    int r;

    /* 修复：api_init 内存不足时这些缓冲是 NULL，先挡住，别在解码器里写空指针 */
    if (!g_rgba || !g_art) return -1;

    /* 网易有的封面是 PNG（扩展名还是 .jpg），sceJpeg 解不了 -> 用自带的解码器 */
    if (len > 4 && jpg[0] == 0x89 && jpg[1] == 'P' && jpg[2] == 'N' && jpg[3] == 'G') {
        return decode_art_png(jpg, len);
    }

    if (jpeg_read_size(jpg, len, &w, &h, &progressive) < 0 || w <= 0 || h <= 0) {
        mlog("art not jpeg (%dx%d)", w, h);
        return -1;
    }

    /* Native MV changes the ME codec image. Keep cover JPEG off that shared
     * hardware path in this mode; the existing CPU scaler handles all sizes. */
    if (progressive || g_art_cpu_only || mv_native_requested() || w > ART_MAX || h > ART_MAX) {
        int sw = 0, sh = 0;
        int rc = decode_cover_rgba(jpg, len, ART_MAX, &sw, &sh);

        if (rc < 0) {
            mlog("art: sw jpeg rc=%d (%dx%d)", rc, w, h);
            return -1;
        }
        w = sw;
        h = sh;
        mlog("art: sw jpeg ok %dx%d", w, h);
        goto have_rgba;
    }

    {
        int colour = -1;
        int need = sceJpegGetOutputInfo((unsigned char *)jpg, (SceSize)len, &colour, 0);

        mlog("art: jpeg %dx%d, outputInfo=%d bytes, colour=%d", w, h, need, colour);
    }

    if (sceJpegCreateMJpeg(w, h) < 0) {
        mlog("art: CreateMJpeg FAIL %dx%d", w, h);
        return -1;
    }

    memset(g_rgba, 0, (size_t)w * h * 4);

    r = sceJpegDecodeMJpeg((unsigned char *)jpg, (SceSize)len, g_rgba, 0);
    sceJpegDeleteMJpeg();

    if (r < 0) {
        int sw = 0, sh = 0;

        mlog("DecodeMJpeg = 0x%08X (%dx%d, %d bytes)", r, w, h, len);
        /* 硬件解不了（尺寸/渐进式），再试一次软件解码 */
        if (decode_cover_rgba(jpg, len, ART_MAX, &sw, &sh) == 0) {
            w = sw;
            h = sh;
            mlog("art: sw fallback ok %dx%d", w, h);
            goto have_rgba;
        }
        return -1;
    }

have_rgba:
    rgba_to_565(g_rgba, w, h, g_art);
    g_art_w = w;
    g_art_h = h;
    build_backdrop(g_art, w, h);
    mlog("art: rgba[0..3]=%02X %02X %02X %02X 565 px0=%04X mid=%04X",
         g_rgba[0], g_rgba[1], g_rgba[2], g_rgba[3],
         g_art[0], g_art[(h / 2) * w + w / 2]);
    return 0;
}

/* 封面：先看缓存，没有再下载并写缓存 */
static void job_local_id(const char *q);
static int load_art(const Song *s)
{
    char url[320];
    int len = 0;
    char err[80] = "";

    if (!s || !g_jpg) return -1;

    len = cache_art_read(s->id, g_jpg, JPEG_MAX);
    if (len > 0) {
        mlog("art: from disk cache (%d bytes)", len);
        if (decode_art(g_jpg, len) == 0) {
            mlog("art: cache ok %dx%d", g_art_w, g_art_h);
            return 0;
        }
        /* 图坏了：删掉，别让它一直挡着（日志里 6143/10445 字节的就是这么来的） */
        mlog("art: cache decode failed, drop it");
        cache_art_drop(s->id);
    }

    if (!s->art[0]) return -1;

    /* 有的 picUrl 是 https：PSP 没有 TLS，改成明文 http（同一个 CDN 都支持）。
     * 尺寸用 16 的倍数：PSP 的 JPEG 解码器按 16x16 宏块工作。 */
    char original[240];safe_copy(original,sizeof(original),s->art);
    char *query=strchr(original,'?');if(query)*query=0;
    const char *base=!strncasecmp(original,"https://",8)?original+8:original;
    if(strstr(base,"music.126.net/"))snprintf(url,sizeof(url),"%s%s?imageView&thumbnail=192x192&type=jpg",!strncasecmp(original,"https://",8)?"http://":"",base);
    else snprintf(url,sizeof(url),"%s%s?param=192y192",!strncasecmp(original,"https://",8)?"http://":"",base);

    {
        unsigned int t0 = sceKernelGetSystemTimeLow();
        int r = http_get(url, (char *)g_jpg, JPEG_MAX, &len, 9000, err, sizeof(err));
        int ms = (int)((sceKernelGetSystemTimeLow() - t0) / 1000);

        if (r < 0 && net_reconnect() == 0) {
            sceKernelDelayThread(120 * 1000);
            t0 = sceKernelGetSystemTimeLow();
            r = http_get(url, (char *)g_jpg, JPEG_MAX, &len, 9000, err, sizeof(err));
            ms = (int)((sceKernelGetSystemTimeLow() - t0) / 1000);
        }

        /* 正文比 Content-Length 短 = 被掐断的半张图，宁可重下一次也别存进缓存 */
        {
            int expect = http_get_expect();

            if (r >= 0 && expect > 0 && len < expect) {
                mlog("art: short body %d/%d, retry", len, expect);
                sceKernelDelayThread(150 * 1000);
                t0 = sceKernelGetSystemTimeLow();
                r = http_get(url, (char *)g_jpg, JPEG_MAX, &len, 9000, err, sizeof(err));
                ms = (int)((sceKernelGetSystemTimeLow() - t0) / 1000);
                expect = http_get_expect();
                if (r >= 0 && expect > 0 && len < expect) {
                    mlog("art: still short %d/%d, skip", len, expect);
                    return -1;
                }
            }
        }

        mlog("art: fetch id=%lld r=%d len=%d %dms", s->id, r, len, ms);
        if (r < 0) return -1;
    }

    if (len >= 4)
        mlog("art: magic %02X %02X %02X %02X", g_jpg[0], g_jpg[1], g_jpg[2], g_jpg[3]);

    if (len <= 0 || len >= JPEG_MAX || decode_art(g_jpg, len) < 0) {
        mlog("art: decode failed (%d bytes)", len);
        return -1;
    }

    if (cache_art_write(s->id, g_jpg, len) < 0)
        mlog("art: cache write failed for %lld; decoded image remains ready", s->id);
    mlog("art: ok %dx%d", g_art_w, g_art_h);
    return 0;
}

const unsigned short *api_art(int *w, int *h)
{
    if (w) *w = g_art_w;
    if (h) *h = g_art_h;
    return g_art_w ? g_art : NULL;
}

const unsigned short *api_backdrop(void)
{
    return g_art_w ? g_backdrop : NULL;
}

const unsigned short *api_backdrop_dim(void)
{
    return (g_art_w && g_backdrop_dim) ? g_backdrop_dim : NULL;
}

void api_clear_art(void)
{
    g_art_w = g_art_h = 0;
}

/* ------------------------------------------------------------------ */
/* 歌词                                                                */
/* ------------------------------------------------------------------ */

/* YRC timing is optional and disposable before MV. No per-line arrays in Lyric. */
#define LYRIC_WORD_CAP 640
typedef struct {
    int start_ms, duration_ms;
    unsigned char line, begin, end, reserved;
} LyricWord;
static LyricWord *g_lyric_words;
static int g_lyric_word_block = -1;
static int g_lyric_word_count;

static void lyric_timing_free(void)
{
    g_lyric_word_count = 0;
    if (g_lyric_word_block >= 0) sceKernelFreePartitionMemory(g_lyric_word_block);
    g_lyric_word_block = -1;
    g_lyric_words = NULL;
}

static int lyric_number(const char **cursor, const char *end, int *value)
{
    const char *p = *cursor;
    int n = 0;
    if (p >= end || *p < '0' || *p > '9') return 0;
    while (p < end && *p >= '0' && *p <= '9') {
        int digit = *p++ - '0';
        if (n > (86400000 - digit) / 10) return 0;
        n = n * 10 + digit;
    }
    *cursor = p; *value = n;
    return 1;
}

/* Absolute word timestamps, as returned by NetEase yrc.lyric (not offsets). */
static void parse_yrc(const char *s)
{
    int published = 0;
    if (!s) return;
    while (*s) {
        const char *e = s, *p;
        int stamp, duration, length = 0, words = 0, valid = 1;
        char text[sizeof(g_lyric.text[0])];
        LyricWord local[sizeof(g_lyric.text[0])];
        while (*e && *e != '\r' && *e != '\n') e++;
        p = s;
        if (p >= e || *p++ != '[' || !lyric_number(&p,e,&stamp) || p>=e || *p++!=',' ||
            !lyric_number(&p,e,&duration) || p>=e || *p++!=']') goto next_yrc;
        while (p < e) {
            int start, dur, unused, begin = length;
            const char *word;
            if (*p++!='(' || !lyric_number(&p,e,&start) || p>=e || *p++!=',' ||
                !lyric_number(&p,e,&dur) || p>=e || *p++!=',' ||
                !lyric_number(&p,e,&unused) || p>=e || *p++!=')' ||
                start < stamp || start > stamp + duration || dur > stamp + duration - start ||
                (words && start < local[words-1].start_ms)) { valid=0; break; }
            word = p;
            while (p < e && *p != '(') p++;
            int n = (int)(p-word);
            if (!n || n >= (int)sizeof(text)-length || words >= (int)sizeof(text)) { valid=0; break; }
            memcpy(text+length,word,(size_t)n); length+=n;
            local[words].start_ms=start; local[words].duration_ms=dur;
            local[words].begin=(unsigned char)begin; local[words].end=(unsigned char)length;
            local[words].reserved=0; words++;
        }
        if (!valid || !words || !length) goto next_yrc;
        if (!g_lyric_words) {
            g_lyric_word_block=sceKernelAllocPartitionMemory(2,"lyric_words",PSP_SMEM_High,
                                                           LYRIC_WORD_CAP*sizeof(LyricWord),NULL);
            if (g_lyric_word_block >= 0) g_lyric_words=sceKernelGetBlockHeadAddr(g_lyric_word_block);
            if (!g_lyric_words) { lyric_timing_free(); return; }
        }
        if (!published) {
            g_lyric.count=0; g_lyric.translated_count=0; g_lyric_word_count=0;
            memset(g_lyric.translation,0,sizeof(g_lyric.translation)); published=1;
        }
        if (g_lyric.count >= MAX_LYRIC) return;
        /* Keep all line text; a line exceeding remaining timing capacity stays line-highlighted. */
        text[length]=0;
        g_lyric.ms[g_lyric.count]=stamp;
        memcpy(g_lyric.text[g_lyric.count],text,(size_t)length+1);
        if (duration > 0 && words <= LYRIC_WORD_CAP-g_lyric_word_count) {
            for (int i=0;i<words;i++) {
                local[i].line=(unsigned char)g_lyric.count;
                g_lyric_words[g_lyric_word_count++]=local[i];
            }
        }
        g_lyric.count++;
next_yrc:
        s=e; while (*s=='\n'||*s=='\r') s++;
    }
}

int api_lyric_word_span(int line, int pos_ms, int *completed_bytes, int *word_end_bytes, float *fraction)
{
    int found=0, completed=0, end=0;
    float f=0.0f;
    if (completed_bytes) *completed_bytes=0;
    if (word_end_bytes) *word_end_bytes=0;
    if (fraction) *fraction=0.0f;
    if (!g_lyric_words || line<0 || line>=g_lyric.count) return 0;
    for (int i=0;i<g_lyric_word_count;i++) {
        const LyricWord *w=&g_lyric_words[i];
        if (w->line!=line) continue;
        found=1; end=w->end;
        if (pos_ms < w->start_ms) { end=w->begin; f=0.0f; break; }
        if (w->duration_ms>0 && pos_ms-w->start_ms < w->duration_ms) {
            completed=w->begin; f=(float)(pos_ms-w->start_ms)/w->duration_ms; break;
        }
        completed=w->end; f=0.0f;
    }
    if (completed_bytes) *completed_bytes=completed;
    if (word_end_bytes) *word_end_bytes=end;
    if (fraction) *fraction=f;
    return found;
}

int api_lyric_timing(int line, int pos_ms, int *completed_bytes, float *next_fraction)
{
    return api_lyric_word_span(line,pos_ms,completed_bytes,NULL,next_fraction);
}

static void yrc_cb(void *ud,const char *path,int type,const char *str,long long num)
{
    (void)ud; (void)num;
    if (type==JSON_STR && !strcmp(path,"yrc.lyric")) parse_yrc(str);
}

static void parse_lyric(const char *s)
{
    g_lyric_word_count=0;
    g_lyric.count = 0;g_lyric.translated_count=0;memset(g_lyric.translation,0,sizeof(g_lyric.translation));
    if (!s) return;

    while (*s && g_lyric.count < MAX_LYRIC) {
        if (*s == '[') {
            int mm = 0, ss = 0, cs = 0;
            const char *p = s + 1;

            while (*p >= '0' && *p <= '9') { mm = mm * 10 + (*p - '0'); p++; }
            if (*p == ':') {
                p++;
                while (*p >= '0' && *p <= '9') { ss = ss * 10 + (*p - '0'); p++; }
                if (*p == '.' || *p == ':') {
                    int digits = 0;
                    p++;
                    while (*p >= '0' && *p <= '9' && digits < 3) {
                        cs = cs * 10 + (*p - '0');
                        p++;
                        digits++;
                    }
                    while (digits < 3) { cs *= 10; digits++; }
                }
                if (*p == ']') {
                    p++;
                    {
                        const char *e = p;
                        int n = 0;
                        while (*e && *e != '\n' && *e != '\r') e++;
                        n = (int)(e - p);
                        if (n > 0 && n < (int)sizeof(g_lyric.text[0])) {
                            memcpy(g_lyric.text[g_lyric.count], p, (size_t)n);
                            g_lyric.text[g_lyric.count][n] = 0;
                            g_lyric.ms[g_lyric.count] = mm * 60000 + ss * 1000 + cs;
                            g_lyric.count++;
                        }
                        while (*e == '\r' || *e == '\n') e++;
                        s = e;
                        continue;
                    }
                }
            }
        }
        while (*s && *s != '\n') s++;
        while (*s == '\r' || *s == '\n') s++;
    }

    mlog("lyric lines %d", g_lyric.count);
}

/* Translation lines attach by timestamp, never by service array position. */
static void parse_translation(const char*s){
 if(!s)return;
 while(*s){int mm=0,ss=0,fraction=0,digits=0,used=0;
  if(*s=='['&&sscanf(s,"[%d:%d%n",&mm,&ss,&used)==2&&used>0){const char*p=s+used;
   if(*p=='.'||*p==':'){p++;while(*p>='0'&&*p<='9'&&digits<3){fraction=fraction*10+(*p++-'0');digits++;}while(digits<3){fraction*=10;digits++;}while(*p>='0'&&*p<='9')p++;}
   if(*p==']'&&mm>=0&&mm<=1440&&ss>=0&&ss<60){p++;const char*e=p;while(*e&&*e!='\n'&&*e!='\r')e++;int stamp=mm*60000+ss*1000+fraction;
    int match=-1,best=g_lyric_word_count>0?201:1;for(int i=0;i<g_lyric.count;++i){int delta=abs(g_lyric.ms[i]-stamp);if(delta<best){best=delta;match=i;}}
    for(int i=0;i<g_lyric.count;++i)if(i==match){int n=(int)(e-p);if(n>0){if(n>=(int)sizeof(g_lyric.translation[i]))n=sizeof(g_lyric.translation[i])-1;while(n>0&&((unsigned char)p[n]&0xc0)==0x80)n--;if(!g_lyric.translation[i][0])g_lyric.translated_count++;memcpy(g_lyric.translation[i],p,n);g_lyric.translation[i][n]=0;}break;}
   }
  }
  while(*s&&*s!='\n'&&*s!='\r')s++;while(*s=='\n'||*s=='\r')s++;
 }
}
static void translation_cb(void*ud,const char*path,int type,const char*str,long long num){(void)ud;(void)num;if(type==JSON_STR&&!strcmp(path,"tlyric.lyric"))parse_translation(str);}

const Lyric *api_lyric(void) { return &g_lyric; }

/* ------------------------------------------------------------------ */
/* 任务实现                                                            */
/* ------------------------------------------------------------------ */

static void job_probe(void)
{
    char msg[64] = "";

    mlog("probe start");

    if (net_probe_host("music.163.com", 80, 5000) == 0) {
        post(EV_NET_OK, 0, "");
        return;
    }

    if (net_wlan_switch() == 0) {
        post(EV_NET_FAIL, 0, "wlan off");
        return;
    }

    /* 真机：XMB 里配好的连接不会自动带进来，主动连一遍。
     * 以前只试 1..3，用户把配置放在第 4 个槽位时永远自动连不上，
     * 每次开机都得自己在 WiFi 页按 ○（日志里 apctl connect(2)/(3) 全失败）。 */
    {
        NetProfile list[NET_MAX_PROFILES];
        int count = net_list_profiles(list, NET_MAX_PROFILES);
        int last = net_last_profile();
        int i;

        if (last > 0) {
            if (net_apctl_connect(last, 10) == 0) {
                mlog("apctl %d connected (last used)", last);
                count = 0;   /* already online: skip the rest */
            }
        }
        for (i = 0; i < count; ++i) {
            if (list[i].id == last) continue;
            if (net_apctl_connect(list[i].id, 10) == 0) {
                mlog("apctl %d connected", list[i].id);
                break;
            }
        }
    }

    if (net_probe_host("music.163.com", 80, 6000) == 0) {
        post(EV_NET_OK, 0, "");
        return;
    }

    net_current_name(msg, sizeof(msg));
    mlog("probe failed, profile=%s", msg);
    post(EV_NET_FAIL, 0, msg);
}

static void job_connect(int profile)
{
    int r;

    mlog("connect profile %d", profile);

    if (net_wlan_switch() == 0) {
        post(EV_NET_FAIL, 0, "wlan off");
        return;
    }

    r = net_manual_connect(profile, 20);
    mlog("apctl connect(%d) = %d, state=%d", profile, r, net_apctl_state());

    if (r != 0) {
        post(EV_NET_FAIL, 0, "connect failed");
        return;
    }

    char probe_error[96]={0};int probe_bytes=0;
    if(http_get(API_BASE "/api/song/detail/?ids=%5B0%5D",g_buf,BUF_SIZE-1,&probe_bytes,6000,probe_error,sizeof(probe_error))==0&&probe_bytes>0&&json_validate(g_buf,probe_bytes)==0){
        mlog("net manual: HTTP response verified bytes=%d",probe_bytes);post(EV_NET_OK,0,"");
    }else{mlog("net manual: HTTP readiness failed %s",probe_error);post(EV_NET_FAIL,0,"WiFi已关联，但服务未响应，请重试");}
}

static void job_chart(int index)
{
    char url[256];
    char err[80] = "";
    ListCtx ctx;

    memset(&g_pending, 0, sizeof(g_pending));
    ctx.list = &g_pending;
    ctx.last_idx = -1;

    snprintf(url, sizeof(url), API_BASE "/api/v3/playlist/detail?id=%lld&n=100&s=0",
             g_charts[index].id);

    if (fetch(url, err, sizeof(err)) < 0) {
        /* 修复：被播放/搜索打断时也回事件（消息留空），别让首页一直等榜单 */
        post(EV_LIST_FAIL, index, g_abort ? "" : (err[0] ? err : "网络错误"));
        return;
    }

    json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);

    if (g_pending.count == 0) {
        /* 有些榜单 id 在 v3 接口里返回空，退回老接口再拉一次 */
        memset(&g_pending, 0, sizeof(g_pending));
        ctx.last_idx = -1;
        snprintf(url, sizeof(url), API_BASE "/api/playlist/detail?id=%lld", g_charts[index].id);
        if (fetch(url, err, sizeof(err)) < 0) {
            post(EV_LIST_FAIL, index, g_abort ? "" : (err[0] ? err : "没有歌曲"));
            return;
        }
        json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);
    }

    if (g_pending.count == 0) {
        post(EV_LIST_FAIL, index, "没有歌曲");
        return;
    }

    g_pending.kind = LK_CHART;
    g_pending.chart_id = g_charts[index].id;
    g_list_total_hint = g_pending.count;
    if (!g_pending.title[0]) safe_copy(g_pending.title, sizeof(g_pending.title),
                                       g_charts[index].name);

    mlog("chart %s: %d songs", g_pending.title, g_pending.count);
    post(EV_LIST_OK, index, g_pending.title);
}

static void count_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    (void)str;
    if (type == JSON_NUM && !strcmp(path, "result.songCount")) *(int *)ud = (int)num;
}

static void job_search(const char *q)
{
    char enc[400];
    char url[720];
    char err[80] = "";
    ListCtx ctx;
    int append = g_search_append;
    int offset = append ? g_search_off : 0;

    if (!append) {
        g_search_off = 0;
        g_search_total = 0;
        safe_copy(g_search_q, sizeof(g_search_q), q);
    }

    memset(&g_pending, 0, sizeof(g_pending));
    ctx.list = &g_pending;
    ctx.last_idx = -1;
    ctx.base = 0;

    net_url_encode(q, enc, sizeof(enc));
    snprintf(url, sizeof(url), API_BASE "/api/cloudsearch/pc?s=%s&type=1&limit=%d&offset=%d",
             enc, SEARCH_PAGE, offset);

    if (fetch(url, err, sizeof(err)) < 0) {
        /* 修复：被播放/新搜索打断时也要回 EV_LIST_MORE / EV_LIST_FAIL，
         * 否则搜索页的 list_more / search_state 永远清不掉 */
        post(append ? EV_LIST_MORE : EV_LIST_FAIL, 0,
             g_abort ? "" : (err[0] ? err : "网络错误"));
        return;
    }

    json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);
    {
        int total = 0;

        json_walk(g_buf, (int)strlen(g_buf), count_cb, &total);
        if (total > 0) g_search_total = total;
    }

    /* 补充第三方结果：网易的公开搜索索引对热门华语歌手经常先给翻唱/他人作品，
     * 酷我搜索能拿到原唱条目。只在第一页补，去重后追加，标「酷我」。 */
    if (!append && g_pending.count < MAX_SONGS) {
        Song extra[10];
        int room = MAX_SONGS - g_pending.count;
        int n;

        if (room > 10) room = 10;
        n = alt_kuwo_search_songs(g_search_q, extra, room);
        for (int i = 0; i < n; ++i) {
            int dup = 0;

            for (int j = 0; j < g_pending.count; ++j) {
                if (!strcmp(g_pending.songs[j].name, extra[i].name) &&
                    !strcmp(g_pending.songs[j].artist, extra[i].artist)) { dup = 1; break; }
            }
            if (!dup && g_pending.count < MAX_SONGS) g_pending.songs[g_pending.count++] = extra[i];
        }
        if (n > 0) mlog("search: kuwo rows=%d total=%d", n, g_pending.count);
    }

    if (g_pending.count == 0) {
        if (append) post(EV_LIST_MORE, 0, "");
        else post(EV_LIST_FAIL, 0, "没有找到");
        return;
    }

    g_pending.kind = LK_SEARCH;
    safe_copy(g_pending.title, sizeof(g_pending.title), g_search_q);
    g_list_total_hint = g_search_total;

    if (append) {
        g_search_off += g_pending.count;
        mlog("search more %s: +%d (off=%d/%d)", g_search_q, g_pending.count, g_search_off,
             g_search_total);
        post(EV_LIST_MORE, 1, "");
    } else {
        g_search_off = g_pending.count;
        mlog("search %s: %d/%d songs", g_search_q, g_pending.count, g_search_total);
        post(EV_LIST_OK, 0, g_pending.title);
    }
}

/* 搜索联想：拼音打进去，服务端给候选（歌手/歌名/专辑），相当于一个"云输入法"。
 * 老接口返回的是 result.artists / result.songs / result.albums，没有 allMatch。 */
static void sug_collect(void *ud, const char *path, int type, const char *str, long long num)
{
    const char *seg[6];
    int len[6];
    int n;
    const char *sec = (const char *)ud;
    int i;

    (void)num;
    if (type != JSON_STR || !str || !str[0]) return;
    n = json_split_path(path, seg, len, 6);
    if (n != 4) return;
    if (!seg_is(seg[0], len[0], "result")) return;
    if (!seg_is(seg[1], len[1], sec)) return;
    if (!seg_is(seg[3], len[3], "name")) return;

    for (i = 0; i < g_sug_n; ++i)
        if (!strcmp(g_sug[i], str)) return;      /* 去重 */
    if (g_sug_n >= SUGGEST_MAX) return;
    safe_copy(g_sug[g_sug_n], sizeof(g_sug[0]), str);
    g_sug_n++;
}

static void job_suggest(const char *q)
{
    char enc[400];
    char url[640];
    char err[80] = "";
    int len;

    if (!q[0]) {
        g_sug_n = 0;
        post(EV_SUGGEST_OK, 0, "");
        return;
    }

    net_url_encode(q, enc, sizeof(enc));
    snprintf(url, sizeof(url), API_BASE "/api/search/suggest/web?s=%s", enc);

    if (fetch(url, err, sizeof(err)) < 0) return;

    len = (int)strlen(g_buf);
    /* Validate the entire response before a callback can mutate candidates. */
    if (json_validate(g_buf, len) < 0) return;
    g_sug_n = 0;
    json_walk(g_buf, len, sug_collect, (void *)"artists");   /* 歌手优先：拼音一般就是人名 */
    json_walk(g_buf, len, sug_collect, (void *)"songs");
    json_walk(g_buf, len, sug_collect, (void *)"albums");

    mlog("suggest %s: %d (%s | %s)", q, g_sug_n, g_sug[0], g_sug_n > 1 ? g_sug[1] : "-");
    post(EV_SUGGEST_OK, 0, "");
}
/* ------------------------------------------------------------------ */
/* 扫码登录                                                            */
/* ------------------------------------------------------------------ */

#define LOGIN_FILE  APP_DIR "login.txt"
#define LOGIN_PART  APP_DIR "login.txt.part"
#define LOGIN_BACKUP APP_DIR "login.txt.bak"

static char g_qr_key[64];
static char g_qr_cookie[1280];
static unsigned int g_qr_generation;
static unsigned int g_qr_account_generation;
static char g_qr_url[160];
static int g_qr_state;            /* 800 过期 / 801 待扫 / 802 待确认 / 803 成功 */
static unsigned char g_qr_mat[57 * 57];
static int g_qr_dim;

static char g_login_cookie[1280];
static char g_login_name[48];
static long long g_login_uid;
static int g_login_vip;
static int g_logged_in;
static long long g_liked_pl;
static int g_liked_total;
static volatile unsigned int g_account_generation = 1;
static volatile unsigned int g_like_request_generation;
static long long g_liked_cache_uid;

static int cookie_import(void);
typedef struct { long long uid; int vip, code; char name[48]; } LoginAccount;
static void account_candidate_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    LoginAccount *a = (LoginAccount *)ud;
    if (type == JSON_NUM && !strcmp(path, "code")) a->code = (int)num;
    else if (type == JSON_NUM && !strcmp(path, "profile.userId")) a->uid = num;
    else if (type == JSON_NUM && !strcmp(path, "profile.vipType")) a->vip = (int)num;
    else if (type == JSON_STR && !strcmp(path, "profile.nickname")) safe_copy(a->name, sizeof(a->name), str);
}


/* ---- 登录凭据的读写：直接用 sceIo，绕开 newlib stdio ----
 * 真机上 newlib 的 fopen/fgets 在启动阶段偶发卡死（PPSSPP 复现不了），
 * 而登录文件只是 1KB 以内的小文件，直接读整个文件再自己拆行最稳。 */
static int login_parse(char *buf, int commit)
{
    char cookie[1280] = "", name[48] = "";
    long long uid = 0;
    int vip = 0, have_uid = 0, have_cookie = 0;
    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t len;
        if (!nl) return -1;
        *nl = 0;
        len = strlen(p);
        if (len && p[len-1] == '\r') p[--len] = 0;
        if (!strncmp(p, "cookie=", 7)) {
            if (have_cookie || len <= 7 || len-7 >= sizeof(cookie)) return -1;
            safe_copy(cookie, sizeof(cookie), p+7); have_cookie = 1;
        } else if (!strncmp(p, "name=", 5)) {
            if (len-5 >= sizeof(name)) return -1;
            safe_copy(name, sizeof(name), p+5);
        } else if (!strncmp(p, "uid=", 4)) {
            char *end; uid = strtoll(p+4, &end, 10);
            if (end == p+4 || *end || uid <= 0) return -1;
            have_uid = 1;
        } else if (!strncmp(p, "vip=", 4)) vip = atoi(p+4);
        p = nl+1;
    }
    if (!have_cookie || !have_uid) return -1;
    if (commit) {
        safe_copy(g_login_cookie, sizeof(g_login_cookie), cookie);
        safe_copy(g_login_name, sizeof(g_login_name), name);
        g_login_uid = uid; g_login_vip = vip;
    }
    return 0;
}

static int login_read_path(const char *path, char *buf, int cap);

static int login_save(void)
{
    char buf[1600];
    int n;
    unsigned int generation;
    if (!lock_take(g_lock)) return -1;
    generation = g_account_generation;
    if (g_login_uid <= 0 || !g_login_cookie[0] ||
        strchr(g_login_cookie, '\n') || strchr(g_login_cookie, '\r') ||
        strchr(g_login_name, '\n') || strchr(g_login_name, '\r')) {
        lock_give(g_lock); return -1;
    }
    n = snprintf(buf, sizeof(buf), "cookie=%s\nname=%s\nuid=%lld\nvip=%d\n",
                 g_login_cookie, g_login_name, g_login_uid, g_login_vip);
    lock_give(g_lock);
    if (n <= 0 || n >= (int)sizeof(buf)) return -1;
    char old[1600];
    io_lock();
    if (generation != api_account_generation()) { io_unlock(); return -1; }
    int valid = login_read_path(LOGIN_FILE, old, sizeof(old)) > 0 && login_parse(old, 0) == 0;
    if (session_replace(LOGIN_FILE, LOGIN_PART, LOGIN_BACKUP, buf, n, valid) < 0) {
        io_unlock();
        mlog("login: session save failed; current session retained");
        return -1;
    }
    io_unlock();
    return 0;
}

static int login_read_path(const char *path, char *buf, int cap)
{
    int fd, n = 0, r = 0;
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    while (n < cap - 1) {
        r = sceIoRead(fd, buf + n, (SceSize)(cap - 1 - n));
        if (r <= 0) break;
        n += r;
    }
    if (sceIoClose(fd) < 0 || r < 0 || n <= 0 || n >= cap - 1) return -1;
    buf[n] = 0;
    return n;
}

static void login_load(void)
{
    char buf[1600];
    int ok = login_read_path(LOGIN_FILE, buf, sizeof(buf)) > 0 && login_parse(buf, 1) == 0;
    if (!ok) ok = login_read_path(LOGIN_BACKUP, buf, sizeof(buf)) > 0 && login_parse(buf, 1) == 0;
    if (ok) {
        g_logged_in = 1;
        net_set_cookie(g_login_cookie);
        return;
    }
    cookie_import();
}

/* 从记忆棒上的 cookie.txt 导入登录态（PC 浏览器里复制 MUSIC_U 存进来即可，
 * 不用在 PSP 上打一长串字符；这条路不经过风控） */
#define COOKIE_FILE APP_DIR "cookie.txt"

static int cookie_import(void)
{
    char buf[1400];
    char *p;
    size_t n = 0;
    int fd;

    fd = sceIoOpen(COOKIE_FILE, PSP_O_RDONLY, 0);
    if (fd < 0) {
        mlog("cookie: no cookie.txt");
        return 0;
    }
    {
        int r = sceIoRead(fd, buf, sizeof(buf) - 1);

        if (r > 0) n = (size_t)r;
    }
    sceIoClose(fd);
    buf[n] = 0;

    /* 去掉换行/空白 */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
    if (!buf[0]) return 0;

    p = strstr(buf, "MUSIC_U=");
    if (!p) {
        mlog("cookie.txt: 没找到 MUSIC_U=");
        return 0;
    }
    if (strlen(p) >= sizeof(g_login_cookie)) return 0;
    safe_copy(g_login_cookie, sizeof(g_login_cookie), p);
    g_logged_in = 1;
    net_set_cookie(g_login_cookie);
    /* Persist only after account identity has been verified. */
    mlog("cookie.txt: imported %d bytes", (int)strlen(g_login_cookie));
    return 1;
}

static void job_account(void)
{
    char err[80] = "";
    LoginAccount a;
    unsigned int generation = api_account_generation();
    if (!g_logged_in) return;
    memset(&a, 0, sizeof(a));
    if (fetch(API_BASE "/api/nuser/account/get", err, sizeof(err)) < 0 ||
        json_walk(g_buf, (int)strlen(g_buf), account_candidate_cb, &a) < 0 ||
        a.code != 200 || a.uid <= 0) {
        post(EV_QR_STATE, -1, "账号验证失败，已保留本地会话");
        return;
    }
    if (!lock_take(g_lock)) return;
    if (generation != g_account_generation) { lock_give(g_lock); return; }
    if (g_login_uid != a.uid) {
        ++g_account_generation; g_liked_pl = 0; g_liked_total = 0; g_liked_cache_uid = 0;
    }
    g_login_uid = a.uid; g_login_vip = a.vip;
    safe_copy(g_login_name, sizeof(g_login_name), a.name);
    lock_give(g_lock);
    if (login_save() < 0) post(EV_INFO, 0, "本次登录未保存");
    post(EV_LOGIN_OK, 0, g_login_name);
}

void api_logout(void)
{
    memset(&g_daily_preview,0,sizeof(g_daily_preview));
    if (!lock_take(g_lock)) return;
    g_logged_in = 0;
    ++g_account_generation;
    ++g_qr_generation;
    g_qr_cookie[0] = g_qr_key[0] = 0;
    g_liked_pl = 0;
    g_liked_total = 0;
    g_liked_cache_uid = 0;
    g_login_cookie[0] = 0;
    g_login_name[0] = 0;
    g_login_uid = 0;
    g_login_vip = 0;
    lock_give(g_lock);
    net_set_cookie("");
    /* 修复：删文件也走全局 stdio 锁，别和别的线程的 fopen/fclose 撞上 */
    io_lock();
    sceIoRemove(LOGIN_FILE);
    sceIoRemove(LOGIN_PART);
    sceIoRemove(LOGIN_BACKUP);
    sceIoRemove(COOKIE_FILE);
    io_unlock();
    mlog("login: logged out");
}

static void qr_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    (void)ud;
    if (type == JSON_STR && !strcmp(path, "unikey")) {
        safe_copy(g_qr_key, sizeof(g_qr_key), str);
    } else if (type == JSON_NUM && !strcmp(path, "code")) {
        g_qr_state = (int)num;
    }
}

static void job_qr_new(unsigned int request_generation)
{
    char err[80] = "";

    if (!lock_take(g_lock)) return;
    unsigned int account = g_qr_account_generation;
    if (request_generation != g_qr_generation || account != g_account_generation) { lock_give(g_lock); return; }
    g_qr_key[0] = 0;
    lock_give(g_lock);
    g_qr_state = 0;
    g_qr_dim = 0;

    if (fetch(API_BASE "/api/login/qrcode/unikey?type=1", err, sizeof(err)) < 0) {
        if (!g_abort) post(EV_QR_STATE, -1, err[0] ? err : "网络错误");
        return;
    }
    if (account != api_account_generation() || request_generation != g_qr_generation) return;
    json_walk(g_buf, (int)strlen(g_buf), qr_cb, NULL);
    if (!g_qr_key[0]) {
        post(EV_QR_STATE, -1, "取二维码失败");
        return;
    }

    /* 把 unikey 那一次下发的 Cookie（NMTID 等）带上，风控会看这个 */
    if (net_last_set_cookie()[0]) {
        safe_copy(g_qr_cookie, sizeof(g_qr_cookie), net_last_set_cookie());
        mlog("login: QR seed cookie received");
    }

    snprintf(g_qr_url, sizeof(g_qr_url), "https://music.163.com/login?codekey=%s", g_qr_key);
    if (qr_encode(g_qr_url, g_qr_mat, 57, &g_qr_dim) < 0) {
        post(EV_QR_STATE, -1, "二维码生成失败");
        return;
    }

    g_qr_state = 801;
    mlog("login: qr ready dim=%d", g_qr_dim);
    post(EV_QR_NEW, 0, g_qr_key);
}

static void job_qr_poll(unsigned int request_generation)
{
    char err[80] = "";
    char url[320];

    if (!lock_take(g_lock)) return;
    unsigned int generation = g_qr_account_generation;
    if (!g_qr_key[0] || request_generation != g_qr_generation || generation != g_account_generation) { lock_give(g_lock); return; }
    lock_give(g_lock);
    snprintf(url, sizeof(url), API_BASE "/api/login/qrcode/client/login?key=%s&type=1", g_qr_key);
    net_set_cookie(g_qr_cookie);
    int fetched = fetch(url, err, sizeof(err));
    net_set_cookie(g_login_cookie);
    if (generation != api_account_generation() || request_generation != g_qr_generation) return;
    if (fetched < 0) {
        if (!g_abort) post(EV_QR_STATE, g_qr_state, err[0] ? err : "网络错误");
        return;
    }

    {
        int old = g_qr_state;

        g_qr_state = 0;
        json_walk(g_buf, (int)strlen(g_buf), qr_cb, NULL);

        /* 拿到 Set-Cookie 就算成功（比 code 可靠）；没有再看 code */
        {
            const char *ck = net_last_set_cookie();

            if (ck && strstr(ck, "MUSIC_U=")) {
                char candidate_cookie[1280];
                LoginAccount a;
                if (strlen(ck) >= sizeof(candidate_cookie)) {
                    post(EV_QR_STATE, -1, "登录凭据过长"); return;
                }
                safe_copy(candidate_cookie, sizeof(candidate_cookie), ck);
                memset(&a, 0, sizeof(a));
                net_set_cookie(candidate_cookie);
                if (fetch(API_BASE "/api/nuser/account/get", err, sizeof(err)) < 0 ||
                    json_walk(g_buf, (int)strlen(g_buf), account_candidate_cb, &a) < 0 ||
                    a.code != 200 || a.uid <= 0) {
                    net_set_cookie(g_login_cookie);
                    post(EV_QR_STATE, -1, "新账号验证失败，已保留原会话"); return;
                }
                if (!lock_take(g_lock)) { net_set_cookie(g_login_cookie); return; }
                if (generation != g_account_generation || request_generation != g_qr_generation) {
                    lock_give(g_lock); net_set_cookie(g_login_cookie); return;
                }
                ++g_account_generation;
                g_liked_pl = 0; g_liked_total = 0; g_liked_cache_uid = 0;
                safe_copy(g_login_cookie, sizeof(g_login_cookie), candidate_cookie);
                safe_copy(g_login_name, sizeof(g_login_name), a.name);
                g_login_uid = a.uid; g_login_vip = a.vip; g_logged_in = 1;
                lock_give(g_lock);
                net_set_cookie(g_login_cookie);
                if (login_save() < 0) post(EV_INFO, 0, "本次登录未保存");
                post(EV_LOGIN_OK, 0, g_login_name);
                return;
            }
        }
        if (g_qr_state != old) mlog("login: qr state %d", g_qr_state);
        post(EV_QR_STATE, g_qr_state, "");
    }
}/* ------------------------------------------------------------------ */
/* 云端内容：每日推荐 / 歌单 / 排行榜 / 评论                            */
/* ------------------------------------------------------------------ */

static CardList g_cards;
static void discovery_cards_publish(void);
static CommentList g_comments;
static int g_comment_off;
static int g_comment_total;
static int g_comment_norm_n;      /* 普通评论页条数；-1 表示正在追加 */
static int g_comment_hot_n;
static int g_comment_more;
static int g_comment_page;
static long long g_comment_song;
static long long g_comment_requested_song;
static unsigned int g_comment_request_gen;
static char g_comment_name[128];
static char g_comment_requested_name[128];
static const CommentList g_comments_empty;
static char g_user_avatar[208];


/* ---- 卡片：歌单 / 榜单 / 歌手 ---- */
typedef struct {
    const char *root;
    int last;
    int kind;
} CardCtx;

static void card_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    CardCtx *c = (CardCtx *)ud;
    const char *seg[8];
    int len[8];
    int n = json_split_path(path, seg, len, 8);
    int a = -1, idx;
    Card *it;

    if (n < 3) return;
    for (int i = 0; i < n; ++i)
        if (seg_is(seg[i], len[i], c->root)) a = i;
    if (a < 0 || a + 2 > n) return;

    idx = atoi(seg[a + 1]);
    if (idx < 0 || idx >= MAX_CARDS) return;
    it = &g_cards.items[idx];
    if (idx != c->last) {
        c->last = idx;
        memset(it, 0, sizeof(*it));
        it->kind = c->kind;
    }
    if (idx + 1 > g_cards.count) g_cards.count = idx + 1;

    if (n == a + 3) {
        const char *f = seg[a + 2];
        int fl = len[a + 2];

        if (type == JSON_NUM) {
            if (seg_is(f, fl, "id")) it->id = num;
            else if (seg_is(f, fl, "playCount") || seg_is(f, fl, "subscribedCount")) it->plays = num;
            else if (seg_is(f, fl, "trackCount") || seg_is(f, fl, "musicSize")) it->tracks = (int)num;
        } else if (type == JSON_STR && str) {
            if (seg_is(f, fl, "name")) safe_copy(it->name, sizeof(it->name), str);
            else if (seg_is(f, fl, "coverImgUrl") || seg_is(f, fl, "picUrl") ||
                     seg_is(f, fl, "coverUrl"))
                safe_copy(it->cover, sizeof(it->cover), str);
            else if (seg_is(f, fl, "updateFrequency") || seg_is(f, fl, "copywriter"))
                safe_copy(it->sub, sizeof(it->sub), str);
        }
        return;
    }
    if (n == a + 4 && type == JSON_STR && str) {
        /* creator.nickname / artist.name */
        if (seg_is(seg[a + 2], len[a + 2], "creator") && seg_is(seg[a + 3], len[a + 3], "nickname"))
            safe_copy(it->sub, sizeof(it->sub), str);
        else if (seg_is(seg[a + 2], len[a + 2], "artist") && seg_is(seg[a + 3], len[a + 3], "name"))
            safe_copy(it->sub, sizeof(it->sub), str);
        return;
    }
}

/* ---- 歌单详情里的附加信息已合并进 pl_meta_ids_cb（见 playlist_load 前） ---- */

/* ---- 歌单分页 -------------------------------------------------------
 * 2026 年起 /api/v3/playlist/detail 对普通歌单只回前 10 首（榜单还能回 100），
 * 但会完整回 playlist.trackIds。所以顺序以 trackIds 为准，歌曲详情按页用
 * /api/v3/song/detail?c=[{"id":..}] 拉，一次 20 首。老接口
 * /api/v3/playlist/track/all 已经 404，不能再用来翻页。 */
#define MAX_TRACK_IDS 2500
#define PL_PAGE       20

static long long g_pl_ids[MAX_TRACK_IDS];
static int g_pl_id_n;
static int g_pl_total;
static long long g_liked_pl;      /* 我喜欢的音乐（specialType=5）的 playlist id */
static int g_pl_owner;            /* g_pl_ids 属于哪个歌单（翻页时校验） */
static int g_liked_total;         /* 我喜欢的音乐在云端的曲目数（界面显示用） */
static int g_mv_stream_job; /* Only the source launched by job_mv_download. */
#include "mv_download_state.h"
/* Video loading preference is read through mv_stream_enabled(). */

/* ---- 我喜欢的：ids 数组 ---- */
static long long g_like_ids[MAX_SONGS];
static int g_like_n;
static int g_like_ids_total;
static long long g_cur_playlist;

static void ids_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    const char *seg[4];
    int len[4];
    int n;
    int idx;

    (void)ud;
    (void)str;
    if (type != JSON_NUM) return;
    n = json_split_path(path, seg, len, 4);
    if (n != 2 || !seg_is(seg[0], len[0], "ids")) return;
    idx = atoi(seg[1]);
    if (idx < 0 || idx >= MAX_SONGS) return;
    g_like_ids[idx] = num;
    if (idx + 1 > g_like_n) g_like_n = idx + 1;
}

/* ---- 最近播放：allData[].song ---- */
/* 修复：allData.N.song.id 只有 4 段，以前按 5/6 段判断，id/name 一个都解析不到；
 * 而且每个字段回调都 memset 整首歌，后面的字段把前面的冲掉。
 * 现在 a 取 "song" 段的位置（兼容 data.allData.N.song.xxx 这种多一层前缀），
 * 偏移都相对它算：<field> = a+2，al.* = a+3，ar.N.* = a+4。 */
typedef struct {
    int last_idx;
} PlayedCtx;

static void played_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    PlayedCtx *ctx = (PlayedCtx *)ud;
    const char *seg[8];
    int len[8];
    int n = json_split_path(path, seg, len, 8);
    int a = -1, idx;
    Song *s;

    if (n < 3) return;
    for (int i = 1; i + 1 < n; ++i) {
        if (seg_is(seg[i], len[i], "song")) { a = i; break; }
    }
    if (a < 1) return;

    idx = atoi(seg[a - 1]);
    if (idx < 0 || idx >= MAX_SONGS) return;
    s = &g_pending.songs[idx];
    if (idx != ctx->last_idx) {
        ctx->last_idx = idx;
        memset(s, 0, sizeof(*s));
    }
    if (idx + 1 > g_pending.count) g_pending.count = idx + 1;

    /* song.id / song.name / song.dt / song.fee / song.pop */
    if (n == a + 2) {
        const char *f = seg[a + 1];
        int fl = len[a + 1];

        if (type == JSON_NUM) {
            if (seg_is(f, fl, "id")) s->id = num;
            else if (seg_is(f, fl, "dt") || seg_is(f, fl, "duration")) s->dur_ms = (int)num;
            else if (seg_is(f, fl, "fee")) s->fee = (int)num;
            else if (seg_is(f, fl, "pop")) s->pop = (int)num;
            else if (seg_is(f, fl, "mv") || seg_is(f, fl, "mvid")) s->mv = (int)num;
        } else if (type == JSON_STR && str) {
            if (seg_is(f, fl, "name")) safe_copy(s->name, sizeof(s->name), str);
        }
        return;
    }

    /* song.al.picUrl / song.al.name */
    if (n == a + 3 && type == JSON_STR && str &&
        (seg_is(seg[a + 1], len[a + 1], "al") || seg_is(seg[a + 1], len[a + 1], "album"))) {
        const char *f = seg[a + 2];
        int fl = len[a + 2];

        if (seg_is(f, fl, "picUrl")) safe_copy(s->art, sizeof(s->art), str);
        else if (seg_is(f, fl, "name")) safe_copy(s->album, sizeof(s->album), str);
        return;
    }

    /* song.ar.N.name（M=0 直接写，其余用 / 追加，和 list_cb 一致） */
    if (n == a + 4 && type == JSON_STR && str &&
        (seg_is(seg[a + 1], len[a + 1], "ar") || seg_is(seg[a + 1], len[a + 1], "artists"))) {
        int ai = atoi(seg[a + 2]);

        if (seg_is(seg[a + 3], len[a + 3], "name") && str[0]) {
            int cur = (int)strlen(s->artist);

            if (ai == 0 || cur == 0) {
                safe_copy(s->artist, sizeof(s->artist), str);
            } else if (cur + 1 + (int)strlen(str) < (int)sizeof(s->artist)) {
                s->artist[cur] = '/';
                safe_copy(s->artist + cur + 1, (int)sizeof(s->artist) - cur - 1, str);
            }
        }
    }
}

/* ---- 评论 ---- */
/* 单页连续追加，沿用40槽固定预算；满后停止请求，不增加常驻内存。 */
#define CM_HOT_SLOTS 15
#define CM_PAGE_SIZE 20

typedef struct {
    int last_hot;
    int last_norm;
    int norm_base;
    int norm_added;
    int scan_only;
    int hot_count;
    int norm_count;
    int total;
    int code;
    int more;           /* -1 = 响应未给 more，用条数/total 判断 */
    int keep_hot;
    int skip_norm;      /* 当前响应内重复ID对应的数组下标 */
} CmtCtx;

static void comment_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    CmtCtx *c = (CmtCtx *)ud;
    const char *seg[8];
    int len[8];
    int n = json_split_path(path, seg, len, 8);
    int a = -1, idx, hot = 0, slot;
    Comment *cm;

    if (n == 1) {
        if (type == JSON_NUM && seg_is(seg[0], len[0], "total")) c->total = (int)num;
        else if (type == JSON_NUM && seg_is(seg[0], len[0], "code")) c->code = (int)num;
        else if (seg_is(seg[0], len[0], "more") &&
                 (type == JSON_TRUE || type == JSON_FALSE)) c->more = (type == JSON_TRUE);
        return;
    }
    if (n < 3) return;
    for (int i = 0; i < n; ++i) {
        if (seg_is(seg[i], len[i], "hotComments")) { a = i; hot = 1; break; }
        else if (seg_is(seg[i], len[i], "comments")) { a = i; hot = 0; break; }
    }
    if (a < 0) return;

    if (a + 2 > n) return;
    idx = atoi(seg[a + 1]);
    if (idx < 0) return;

    if (c->scan_only) {
        if (hot) {
            if (idx < CM_HOT_SLOTS && idx + 1 > c->hot_count) c->hot_count = idx + 1;
        } else if (idx + 1 > c->norm_count) c->norm_count = idx + 1;
        return;
    }
    if (hot) {
        if (c->keep_hot || idx >= CM_HOT_SLOTS) return;
        slot = idx;
    } else {
        if (idx >= CM_PAGE_SIZE) return;
        if (idx == c->skip_norm) return;
        slot = c->norm_base + c->norm_added - (idx == c->last_norm ? 1 : 0);
        if (slot >= MAX_COMMENTS) return;
    }

    cm = &g_comments.items[slot];
    if (idx != (hot ? c->last_hot : c->last_norm)) {
        if (hot) {
            c->last_hot = idx;
        } else {
            c->last_norm = idx;
            c->norm_added++;
        }
        memset(cm, 0, sizeof(*cm));
        cm->reply_count=-1; /* absent is unknown, never a fabricated zero */
        cm->hot = hot;
    }
    if (slot + 1 > g_comments.count) g_comments.count = slot + 1;

    {
        const char *f = seg[a + 2];
        int fl = len[a + 2];

        if (type == JSON_NUM && n == a + 3) {
            if (seg_is(f, fl, "commentId")) {
                if (!hot && num > 0) {
                    for (int i = 0; i < slot; ++i) {
                        if (g_comments.items[i].id == num) {
                            c->skip_norm = idx;
                            --c->norm_added;
                            g_comments.count = slot;
                            return;
                        }
                    }
                }
                cm->id = num;
            }
            else if (seg_is(f, fl, "replyCount")) cm->reply_count = (int)num;
            else if (seg_is(f, fl, "likedCount")) cm->liked = (int)num;
            else if (seg_is(f, fl, "time")) cm->time = num;
        }else if(type==JSON_NUM&&n==a+4&&seg_is(f,fl,"showFloorComment")&&seg_is(seg[a+3],len[a+3],"replyCount")){cm->reply_count=num>=0&&num<=2147483647?num:-1;
        } else if (type == JSON_STR && str && n == a + 3) {
            if (seg_is(f, fl, "content")) safe_copy(cm->text, sizeof(cm->text), str);
            else if (seg_is(f, fl, "ipLocation")) safe_copy(cm->ip, sizeof(cm->ip), str);
        } else if (type == JSON_STR && str && n == a + 5 &&
                   seg_is(seg[a + 2], len[a + 2], "ipLocation") &&
                   seg_is(seg[a + 3], len[a + 3], "location")) {
            safe_copy(cm->ip, sizeof(cm->ip), str);
        } else if (type == JSON_STR && str && n == a + 4) {
            if (seg_is(seg[a + 2], len[a + 2], "user")) {
                if (seg_is(seg[a + 3], len[a + 3], "nickname")) safe_copy(cm->user, sizeof(cm->user), str);
                else if (seg_is(seg[a + 3], len[a + 3], "avatarUrl")) safe_copy(cm->avatar, sizeof(cm->avatar), str);
            }
        }
    }
}

/* ---- 用户详情 ---- */
static void user_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    (void)ud;
    if (type == JSON_NUM && !strcmp(path, "level")) g_app.user_level = (int)num;
    else if (type == JSON_NUM && !strcmp(path, "listenSongs")) g_app.user_listen = (int)num;
    else if (type == JSON_STR && !strcmp(path, "profile.avatarUrl"))
        safe_copy(g_user_avatar, sizeof(g_user_avatar), str);
}

/* ---- 各个 job ---- */

static int fetch_json(const char *url, char *err, int errlen)
{
    if (fetch(url, err, errlen) < 0) return -1;
    return 0;
}

/* Parse only the first song into bounded storage. Preview must never borrow
 * g_pending: a foreground list event may still be waiting for the UI to read it. */
static void daily_preview_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    Song *song = (Song *)ud;
    const char *seg[10];
    int len[10], a = -1;
    int n = json_split_path(path, seg, len, 10);
    for (int i = 0; i < n; ++i)
        if (seg_is(seg[i], len[i], "dailySongs") || seg_is(seg[i], len[i], "songs") ||
            seg_is(seg[i], len[i], "tracks")) a = i;
    if (a < 0 || a + 2 >= n || !seg_is(seg[a + 1], len[a + 1], "0")) return;
    if (n == a + 3) {
        if (type == JSON_NUM && seg_is(seg[a + 2], len[a + 2], "id")) song->id = num;
        else if (type == JSON_STR && seg_is(seg[a + 2], len[a + 2], "name"))
            safe_copy(song->name, sizeof(song->name), str);
    } else if (n == a + 4 && type == JSON_STR &&
               (seg_is(seg[a + 2], len[a + 2], "al") || seg_is(seg[a + 2], len[a + 2], "album")) &&
               seg_is(seg[a + 3], len[a + 3], "picUrl")) {
        safe_copy(song->art, sizeof(song->art), str);
    }
}

const Song*api_daily_preview(void){return g_daily_preview.id>0&&g_daily_preview_account==api_account_generation()?&g_daily_preview:NULL;}

void api_request_daily_preview(void)
{
    unsigned now = sceKernelGetSystemTimeLow();
    if (!lock_take(g_lock)) return;
    if (!g_logged_in) { lock_give(g_lock); return; }
    if (g_daily_preview_request_account != g_account_generation) {
        g_daily_preview_request_account = g_account_generation;
        g_daily_preview_pending = g_daily_preview_inflight = g_daily_preview_attempted = 0;
    }
    if (!(g_daily_preview.id > 0 && g_daily_preview_account == g_account_generation) &&
        !g_daily_preview_pending && !g_daily_preview_inflight &&
        (!g_daily_preview_attempted || (unsigned)(now - g_daily_preview_retry_at) >= 60000000u))
        g_daily_preview_pending = 1;
    lock_give(g_lock);
}

static void job_daily_preview(unsigned account)
{
    Song song;
    char err[80] = "";
    int ok = 0;
    memset(&song, 0, sizeof(song));
    if (account == api_account_generation() && g_logged_in &&
        fetch_json(API_BASE "/api/v3/discovery/recommend/songs", err, sizeof(err)) >= 0 && !g_abort) {
        json_walk(g_buf, (int)strlen(g_buf), daily_preview_cb, &song);
        ok = song.id > 0 && song.art[0];
    }
    if (!lock_take(g_lock)) return;
    if (account == g_account_generation && account == g_daily_preview_request_account) {
        g_daily_preview_inflight = 0;
        g_daily_preview_attempted = 1;
        g_daily_preview_retry_at = sceKernelGetSystemTimeLow();
        if (ok && !g_abort && g_logged_in) {
            g_daily_preview = song;
            g_daily_preview_account = account;
        }
    }
    lock_give(g_lock);
}

static void job_daily(void)
{
    unsigned preview_account=api_account_generation();
    char err[80] = "";

    memset(&g_pending, 0, sizeof(g_pending));
    {
        ListCtx ctx;

        ctx.list = &g_pending;
        ctx.last_idx = -1;
        ctx.base = 0;

        if (fetch_json(API_BASE "/api/v3/discovery/recommend/songs", err, sizeof(err)) < 0) {
            /* 修复：被打断也要回事件，避免每日推荐页卡在加载中 */
            post(EV_LIST_FAIL, 0, g_abort ? "" : (err[0] ? err : "网络错误"));
            return;
        }
        json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);
    }
    if (g_pending.count == 0) {
        post(EV_LIST_FAIL, 0, "登录后才能看每日推荐");
        return;
    }
    if(preview_account!=api_account_generation())return;
    g_daily_preview=g_pending.songs[0];g_daily_preview_account=preview_account;
    g_pending.kind = LK_DAILY;
    safe_copy(g_pending.title, sizeof(g_pending.title), "每日推荐");
    g_list_total_hint = g_pending.count;
    mlog("daily: %d songs", g_pending.count);
    post(EV_DAILY_OK, 0, g_pending.title);
}

#define HOME_CARDS_CACHE APP_DIR "cache/home-cards.bin"
/* Homepage owns its recommendation metadata independently of card/detail pages. */
static CardList g_home_cards;
static int home_cards_loaded;
static void home_cards_load(void){
    if(home_cards_loaded)return;home_cards_loaded=1;
    SceUID fd=sceIoOpen(HOME_CARDS_CACHE,PSP_O_RDONLY,0);if(fd<0)return;
    unsigned header[2];CardList saved;int valid=sceIoRead(fd,header,sizeof(header))==(int)sizeof(header)&&header[0]==0x48434c31u&&header[1]==sizeof(saved);
    if(valid)valid=sceIoRead(fd,&saved,sizeof(saved))==(int)sizeof(saved);sceIoClose(fd);
    if(!valid||saved.count<0||saved.count>MAX_CARDS)return;
    saved.title[47]=0;for(int i=0;i<saved.count;++i){saved.items[i].name[63]=0;saved.items[i].cover[519]=0;saved.items[i].sub[47]=0;if(saved.items[i].id<=0)return;}
    g_home_cards=saved;g_cards=saved;mlog("home cards: restored %d cached cards",saved.count);
}
static void home_cards_save(void){
    unsigned header[2]={0x48434c31u,sizeof(CardList)};SceUID fd=sceIoOpen(HOME_CARDS_CACHE ".tmp",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0600);if(fd<0)return;
    int ok=sceIoWrite(fd,header,sizeof(header))==(int)sizeof(header)&&sceIoWrite(fd,&g_cards,sizeof(g_cards))==(int)sizeof(g_cards);if(sceIoClose(fd)<0)ok=0;
    if(ok){sceIoRemove(HOME_CARDS_CACHE);if(sceIoRename(HOME_CARDS_CACHE ".tmp",HOME_CARDS_CACHE)<0)mlog("home cards: cache publish failed");}
}

static void cards_fetch(const char *url, const char *root, int kind, const char *title,
                        int ev)
{
    char err[80] = "";
    CardCtx ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.root = root;
    ctx.last = -1;
    ctx.kind = kind;

    if (fetch_json(url, err, sizeof(err)) < 0) {
        /* 修复：被播放/搜索打断时也要回事件，避免卡片页一直空着 */
        post(EV_LIST_FAIL, 0, g_abort ? "" : (err[0] ? err : "网络错误"));
        return;
    }
    memset(&g_cards,0,sizeof(g_cards));safe_copy(g_cards.title,sizeof(g_cards.title),title);
    json_walk(g_buf, (int)strlen(g_buf), card_cb, &ctx);
    if (g_cards.count == 0) {
        post(EV_LIST_FAIL, 0, "没有内容");
        return;
    }
    for (int i = 0; i < g_cards.count; ++i) g_cards.items[i].kind = kind;
    if(strstr(url,"/api/personalized/playlist")){g_home_cards=g_cards;home_cards_save();}
    if(strstr(url,"/api/playlist/list?"))discovery_cards_publish();
    mlog("cards %s: %d", title, g_cards.count);
    post(ev, 0, g_cards.title);
}

/* 歌单详情一次走完：标题/统计/封面 + 完整 trackIds。
 * （以前分两次 json_walk，200KB 的响应在 PSP 上要多花 100~200ms） */
static void pl_meta_ids_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    const char *seg[8];
    int len[8];
    int n = json_split_path(path, seg, len, 8);

    (void)ud;
    if (n < 2 || !seg_is(seg[0], len[0], "playlist")) return;

    if (n == 2 && type == JSON_STR && seg_is(seg[1], len[1], "name")) {
        safe_copy(g_pending.title, sizeof(g_pending.title), str);
        return;
    }
    if (n == 2 && type == JSON_NUM && seg_is(seg[1], len[1], "trackCount") && num > 0) {
        g_cards.count = 0;
        g_cards.items[0].tracks = (int)num;
        return;
    }
    if (n == 2 && type == JSON_NUM && seg_is(seg[1], len[1], "playCount")) {
        g_cards.items[0].plays = num;
        return;
    }
    if (n == 2 && type == JSON_STR && seg_is(seg[1], len[1], "coverImgUrl")) {
        safe_copy(g_cards.items[0].cover, sizeof(g_cards.items[0].cover), str);
        return;
    }
    if (n == 3 && type == JSON_STR && seg_is(seg[1], len[1], "creator") &&
        seg_is(seg[2], len[2], "nickname")) {
        safe_copy(g_cards.items[0].sub, sizeof(g_cards.items[0].sub), str);
        return;
    }
    if (type == JSON_NUM && n == 4 && seg_is(seg[1], len[1], "trackIds") &&
        seg_is(seg[3], len[3], "id")) {
        int idx = atoi(seg[2]);

        if (idx >= 0 && idx < MAX_TRACK_IDS) {
            g_pl_ids[idx] = num;
            if (idx + 1 > g_pl_id_n) g_pl_id_n = idx + 1;
        }
    }
}

/* 载入一个歌单：先拿 detail（trackIds 完整），歌曲详情先吃 tracks 里已有的，
 * 剩下的滚到底再按页取。返回 0 成功。 */
static int playlist_load(long long id, int kind, const char *title_override)
{
    char url[256];
    char err[80] = "";
    ListCtx ctx;

    memset(&g_pending, 0, sizeof(g_pending));
    memset(&g_cards, 0, sizeof(g_cards));
    ctx.list = &g_pending;
    ctx.last_idx = -1;
    ctx.base = 0;
    g_pl_id_n = 0;
    g_pl_total = 0;
    g_pl_owner = (int)(id & 0x7fffffff);
    g_cur_playlist = id;      /* 续页请求要靠它带 id */

    /* n=20：网易会回前 20 首 + 完整的 trackIds。别要 100 首 ——
     * 「我喜欢的音乐」1000+ 首时，100 首的 JSON + trackIds 会超过 320KB 缓冲，
     * 尾部（正好是 trackIds）被截断，列表就永远只有一页。 */
    snprintf(url, sizeof(url), API_BASE "/api/v3/playlist/detail?id=%lld&n=20&s=0", id);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        /* 被播放/搜索打断时也要回事件，避免歌单页卡在加载中 */
        post(EV_LIST_FAIL, 0, g_abort ? "" : (err[0] ? err : "网络错误"));
        return -1;
    }
    json_walk(g_buf, (int)strlen(g_buf), pl_meta_ids_cb, NULL);
    json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);

    if (g_cards.items[0].cover[0] || g_cards.items[0].tracks > 0) g_cards.count = 1;

    if (g_pending.count == 0 && g_pl_id_n == 0) {
        post(EV_LIST_FAIL, 0, "歌单是空的");
        return -1;
    }
    /* trackIds 是完整清单；tracks 可能只有 20 首（网易的限制） */
    g_pl_total = g_pl_id_n > g_pending.count ? g_pl_id_n : g_pending.count;
    g_list_total_hint = g_pl_total;
    if (kind == LK_LIKED) g_liked_total = g_pl_total;
    g_pending.kind = kind;
    if (title_override && !g_pending.title[0])
        safe_copy(g_pending.title, sizeof(g_pending.title), title_override);
    mlog("playlist %lld: %d/%d songs (ids=%d)", id, g_pending.count, g_pl_total, g_pl_id_n);
    post(EV_SONGS_OK, 0, g_pending.title);
    return 0;
}

static void job_playlist(long long id)
{
    g_cur_playlist = id;
    playlist_load(id, LK_PLAYLIST, NULL);
}

static void job_playlist_more(const char *q)
{
    char c[1024];
    char url[1200];
    char err[80] = "";
    const char *bar = strchr(q, '|');
    long long id = atoll(q);
    int off = bar ? atoi(bar + 1) : 0;
    int o = 0, n = 0;
    ListCtx ctx;

    /* 歌单已经换了：这一页作废，别把别的歌单的歌追加进来。
     * 注意先把 g_pending 清空 —— 不然 main 会把上一页的数据再追加一遍。 */
    if (g_pl_id_n <= 0 || off < 0 || off >= g_pl_id_n || (int)(id & 0x7fffffff) != g_pl_owner) {
        memset(&g_pending, 0, sizeof(g_pending));
        post(EV_LIST_MORE, 0, "");
        return;
    }

    memset(&g_pending, 0, sizeof(g_pending));
    o += snprintf(c + o, sizeof(c) - (size_t)o, "%%5B");
    for (int i = off; i < g_pl_id_n && n < PL_PAGE; ++i, ++n) {
        if (n) o += snprintf(c + o, sizeof(c) - (size_t)o, "%%2C");
        o += snprintf(c + o, sizeof(c) - (size_t)o, "%%7B%%22id%%22%%3A%lld%%7D", g_pl_ids[i]);
    }
    snprintf(c + o, sizeof(c) - (size_t)o, "%%5D");

    snprintf(url, sizeof(url), API_BASE "/api/v3/song/detail?c=%s", c);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        /* 被打断时也必须回 EV_LIST_MORE，否则列表滚到底后 list_more 永远是 1 */
        memset(&g_pending, 0, sizeof(g_pending));
        post(EV_LIST_MORE, 0, g_abort ? "" : (err[0] ? err : ""));
        return;
    }
    ctx.list = &g_pending;
    ctx.last_idx = -1;
    ctx.base = 0;
    json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);
    g_pending.kind = LK_PLAYLIST;
    mlog("playlist more off=%d got=%d", off, g_pending.count);
    post(EV_LIST_MORE, 1, "");
}

/* ---- 我喜欢的音乐：先解析出 specialType=5 的歌单 id，再按普通歌单整份加载 ---- */
static long long g_liked_cand[64];
static unsigned char g_liked_special[64];
static int g_liked_cand_n[64];

static void liked_pl_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    const char *seg[4];
    int len[4];
    int n = json_split_path(path, seg, len, 4);
    int idx;

    (void)ud;
    (void)str;
    if (type != JSON_NUM || n != 3) return;
    if (!seg_is(seg[0], len[0], "playlist")) return;
    idx = atoi(seg[1]);
    if (idx < 0 || idx >= 64) return;
    if (seg_is(seg[2], len[2], "specialType")) {
        if ((int)num == 5) g_liked_special[idx] = 1;
    } else if (seg_is(seg[2], len[2], "id")) {
        g_liked_cand[idx] = num;
    } else if (seg_is(seg[2], len[2], "trackCount")) {
        g_liked_cand_n[idx] = (int)num;
    }
}

static void liked_pl_save(void)
{
    g_liked_cache_uid = g_login_uid;
    FILE *fp = fopen(APP_DIR "liked.txt", "w");

    if (!fp) return;
    fprintf(fp, "%lld %lld %d\n", g_login_uid, g_liked_pl, g_liked_total);
    fclose(fp);
}

static void liked_pl_load(void)
{
    FILE *fp;
    long long uid = 0, id = 0;
    int total = 0;

    if (g_liked_cache_uid == g_login_uid && g_liked_pl > 0) return;
    if (g_liked_cache_uid != g_login_uid) {
        g_liked_pl = 0;
        g_liked_total = 0;
        g_liked_cache_uid = 0;
    }
    fp = fopen(APP_DIR "liked.txt", "r");
    if (!fp) return;
    if (fscanf(fp, "%lld %lld %d", &uid, &id, &total) >= 2 &&
        uid == g_login_uid && id > 0) {
        g_liked_pl = id;
        if (total > 0) g_liked_total = total;
        g_liked_cache_uid = uid;
    }
    fclose(fp);
}

static int liked_pl_resolve(void)
{
    char url[256];
    char err[80] = "";
    long long best = 0;
    long long owner = g_login_uid;
    unsigned int generation = g_account_generation;

    liked_pl_load();
    if (g_liked_pl > 0) return 0;

    memset(g_liked_cand, 0, sizeof(g_liked_cand));
    memset(g_liked_special, 0, sizeof(g_liked_special));
    memset(g_liked_cand_n, 0, sizeof(g_liked_cand_n));
    snprintf(url, sizeof(url), API_BASE "/api/user/playlist?uid=%lld&limit=100&offset=0", g_login_uid);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        mlog("liked: user/playlist failed: %s", err);
        return -1;
    }
    if (owner != g_login_uid || generation != g_account_generation) return -1;
    json_walk(g_buf, (int)strlen(g_buf), liked_pl_cb, NULL);
    for (int i = 0; i < 64; ++i) {
        if (g_liked_special[i] && g_liked_cand[i] > 0) {
            best = g_liked_cand[i];
            g_liked_total = g_liked_cand_n[i] > 0 ? g_liked_cand_n[i] : 0;
            break;
        }
    }
    /* specialType 字段万一缺失：只有名字对不上时才不猜，宁可走老接口 */
    g_liked_pl = best;
    if (g_liked_pl > 0) {
        mlog("liked playlist = %lld", g_liked_pl);
        liked_pl_save();
        return 0;
    }
    return -1;
}

/* 只取「我喜欢的音乐」的曲目数（我的页面显示用），不加载歌曲详情。
 * limit=1 只回第一个歌单，响应很小；结果写进 liked.txt，下次开机直接显示。 */
static void job_liked_total(void)
{
    char url[256];
    char err[80] = "";
    long long owner = g_login_uid;
    unsigned int generation = g_account_generation;

    if (!g_logged_in) return;
    liked_pl_load();
    memset(g_liked_cand, 0, sizeof(g_liked_cand));
    memset(g_liked_special, 0, sizeof(g_liked_special));
    memset(g_liked_cand_n, 0, sizeof(g_liked_cand_n));
    snprintf(url, sizeof(url), API_BASE "/api/user/playlist?uid=%lld&limit=1&offset=0",
             g_login_uid);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        mlog("liked total: %s", err);
        return;
    }
    if (owner != g_login_uid || generation != g_account_generation) return;
    json_walk(g_buf, (int)strlen(g_buf), liked_pl_cb, NULL);
    for (int i = 0; i < 64; ++i) {
        if (g_liked_special[i] && g_liked_cand[i] > 0) {
            g_liked_pl = g_liked_cand[i];
            g_liked_total = g_liked_cand_n[i] > 0 ? g_liked_cand_n[i] : 0;
            break;
        }
    }
    if (g_liked_pl > 0) {
        liked_pl_save();
        mlog("liked total=%d id=%lld", g_liked_total, g_liked_pl);
    }
}

static void job_liked(void)
{    char url[512];
    char err[80] = "";
    char ids[4096];
    int o = 0;

    if (!g_logged_in) {
        post(EV_LIST_FAIL, 0, "登录后才能看「我喜欢的音乐」");
        return;
    }

    /* 首选：把「我喜欢的音乐」当成一个普通歌单整份加载（有封面/完整顺序） */
    if (liked_pl_resolve() == 0 && g_liked_pl > 0) {
        mlog("liked: load playlist %lld", g_liked_pl);
        if (playlist_load(g_liked_pl, LK_LIKED, "我喜欢的音乐") == 0) return;
    }

    /* 兜底：老的 like/get + 逐批 song/detail */
    memset(&g_pending, 0, sizeof(g_pending));
    g_like_n = 0;
    snprintf(url, sizeof(url), API_BASE "/api/song/like/get?uid=%lld", g_login_uid);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        /* 被播放/新搜索打断时也要回事件，否则列表一直转圈 */
        post(EV_LIST_FAIL, 0, g_abort ? "" : (err[0] ? err : "网络错误"));
        return;
    }
    json_walk(g_buf, (int)strlen(g_buf), ids_cb, NULL);
    if (g_like_n == 0) {
        post(EV_LIST_FAIL, 0, "还没有喜欢的歌");
        return;
    }

    /* 分两批取 100 首，单次 URL 控制在 1KB 以内。
     * 修复：删掉了循环后面那次用最后一批 ids 从头再解析一遍的请求 ——
     * 它 base=0 重新写，会把前 50 首覆盖掉，排序也乱。 */
    g_like_ids_total = 0;
    {
        int start = g_like_n > 100 ? g_like_n - 100 : 0;
        int batches = ((g_like_n - start) + 49) / 50;
        int bi;

        memset(&g_pending, 0, sizeof(g_pending));
        for (bi = 0; bi < batches; ++bi) {

            int from = start + bi * 50;
            int to = from + 50;

            if (to > g_like_n) to = g_like_n;
            o = 0;
            o += snprintf(ids + o, sizeof(ids) - (size_t)o, "[");
            for (int i = from; i < to && o < (int)sizeof(ids) - 24; ++i) {
                if (i > from) o += snprintf(ids + o, sizeof(ids) - (size_t)o, "%s", ",");
                o += snprintf(ids + o, sizeof(ids) - (size_t)o, "%lld", g_like_ids[i]);
            }
            snprintf(ids + o, sizeof(ids) - (size_t)o, "]");

            {
                char enc[5000];
                char u2[4600];
                ListCtx bctx;

                net_url_encode(ids, enc, sizeof(enc));
                snprintf(u2, sizeof(u2), API_BASE "/api/song/detail?ids=%s", enc);
                if (fetch_json(u2, err, sizeof(err)) < 0) {
                    if (g_abort) break;     /* 被打断：别再把剩下的批次跑完 */
                    continue;
                }
                bctx.list = &g_pending;
                bctx.last_idx = -1;
                bctx.base = g_like_ids_total;
                json_walk(g_buf, (int)strlen(g_buf), list_cb, &bctx);
                g_like_ids_total = g_pending.count;
            }
        }
    }

    /* 打断了就丢掉半份结果，只回一个空事件让 UI 收尾 */
    if (g_abort) {
        post(EV_LIST_FAIL, 0, "");
        return;
    }

    /* 修复：list_cb 已经挡了 idx >= MAX_SONGS，这里再兜一层 */
    if (g_pending.count > MAX_SONGS) g_pending.count = MAX_SONGS;

    if (g_pending.count == 0) {
        post(EV_LIST_FAIL, 0, "没有数据");
        return;
    }
    g_pending.kind = LK_LIKED;
    safe_copy(g_pending.title, sizeof(g_pending.title), "我喜欢的音乐");
    g_list_total_hint = g_pending.count;
    mlog("liked: %d songs", g_pending.count);
    post(EV_SONGS_OK, 0, g_pending.title);
}

static void job_played(void)
{
    char url[256];
    char err[80] = "";
    PlayedCtx pctx;

    memset(&g_pending, 0, sizeof(g_pending));
    snprintf(url, sizeof(url), API_BASE "/api/v1/play/record?uid=%lld&type=0", g_login_uid);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        /* 修复：被播放/搜索打断时也要回事件，否则 UI 一直等这个列表 */
        post(EV_LIST_FAIL, 0, g_abort ? "" : (err[0] ? err : "网络错误"));
        return;
    }
    pctx.last_idx = -1;
    json_walk(g_buf, (int)strlen(g_buf), played_cb, &pctx);
    if (g_pending.count == 0) {
        post(EV_LIST_FAIL, 0, "没有播放记录");
        return;
    }
    g_pending.kind = LK_PLAYED;
    safe_copy(g_pending.title, sizeof(g_pending.title), "最近播放");
    g_list_total_hint = g_pending.count;
    mlog("played: %d songs", g_pending.count);
    post(EV_SONGS_OK, 0, g_pending.title);
}

static void job_artist_songs(long long id, const char *name)
{
    char url[256];
    char err[80] = "";
    ListCtx ctx;

    memset(&g_pending, 0, sizeof(g_pending));
    snprintf(url, sizeof(url),
             API_BASE "/api/v1/artist/songs?id=%lld&limit=60&offset=0&order=hot", id);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        /* 修复：被打断也要回事件，避免歌手页一直等 */
        post(EV_LIST_FAIL, 0, g_abort ? "" : (err[0] ? err : "网络错误"));
        return;
    }
    ctx.list = &g_pending;
    ctx.last_idx = -1;
    ctx.base = 0;
    json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);
    if (g_pending.count == 0) {
        post(EV_LIST_FAIL, 0, "没有歌曲");
        return;
    }
    g_pending.kind = LK_ARTIST;
    safe_copy(g_pending.title, sizeof(g_pending.title), name);
    g_list_total_hint = g_pending.count;
    post(EV_SONGS_OK, 0, g_pending.title);
}

static void job_comments(long long song_id, int offset, unsigned int generation)
{
    char url[256];
    char err[80] = "";
    CmtCtx ctx;
    int body_len;

    if (generation != g_comment_request_gen || song_id != g_comment_requested_song) return;
    /* 不重复取已耗尽或过期的普通页。初次 offset=0 始终允许刷新。 */
    if (offset > 0 && (song_id != g_comment_song ||
        (offset >= g_comment_off && (!g_comment_more || offset != g_comment_off)))) {
        post_ex(EV_COMMENTS_OK, offset > 0 ? 1 : 0, song_id, 0, generation, "");
        return;
    }
    snprintf(url, sizeof(url),
             API_BASE "/api/v1/resource/comments/R_SO_4_%lld?limit=%d&offset=%d",
             song_id, CM_PAGE_SIZE, offset);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        if (generation == g_comment_request_gen && song_id == g_comment_requested_song) {
            g_comment_norm_n = 0;
            post_ex(EV_LIST_FAIL, 0, song_id, 0, generation, g_abort ? "" : (err[0] ? err : "网络错误"));
        }
        return;
    }
    if (generation != g_comment_request_gen || song_id != g_comment_requested_song) return;
    /* 先验证并计数，失败保留当前页；不额外分配 CommentList 或 JSON DOM。 */
    memset(&ctx, 0, sizeof(ctx));
    ctx.scan_only = 1;
    ctx.total = -1;
    ctx.more = -1;
    body_len = (int)strlen(g_buf);
    if (json_walk(g_buf, body_len, comment_cb, &ctx) < 0 ||
        ctx.code != 200) {
        if (generation == g_comment_request_gen && song_id == g_comment_requested_song) {
            g_comment_norm_n = 0;
            post_ex(EV_LIST_FAIL, 0, song_id, 0, generation, "评论响应错误");
        }
        return;
    }
    if (!lock_take(g_lock)) return;
    if (generation != g_comment_request_gen || song_id != g_comment_requested_song) { lock_give(g_lock); return; }
    if (offset > 0 && ctx.norm_count == 0) {
        g_comment_more = 0;
        g_comment_norm_n = 0;
        lock_give(g_lock);
        post_ex(EV_COMMENTS_OK, offset > 0 ? 1 : 0, song_id, 0, generation, "");
        return;
    }
    if (offset == 0) {
        memset(&g_comments, 0, sizeof(g_comments));
        g_comment_hot_n = ctx.hot_count;
        g_comment_page = 0;
        g_comment_song = song_id;
        safe_copy(g_comment_name, sizeof(g_comment_name), g_comment_requested_name);
    }
    g_comment_norm_n = ctx.norm_count > CM_PAGE_SIZE ? CM_PAGE_SIZE : ctx.norm_count;
    if (offset == 0) g_comments.count = g_comment_hot_n;
    if (ctx.total >= 0) g_comment_total = ctx.total;
    else if (offset == 0) g_comment_total = 0;
    g_comment_off = offset + ctx.norm_count;
    g_comment_more = ctx.norm_count > 0 &&
        (ctx.more >= 0 ? ctx.more : ctx.norm_count >= CM_PAGE_SIZE);
    if (g_comment_total > 0 && g_comment_off >= g_comment_total) g_comment_more = 0;
    ctx.scan_only = 0;
    ctx.keep_hot = offset > 0;
    ctx.norm_base = offset > 0 ? g_comments.count : g_comment_hot_n;
    ctx.skip_norm = -1;
    ctx.last_hot = ctx.last_norm = -1;
    json_walk(g_buf, body_len, comment_cb, &ctx);
    if (g_comments.count >= MAX_COMMENTS) g_comment_more = 0;
    g_comments.total = g_comment_total;
    g_comment_page = offset / CM_PAGE_SIZE + 1;
    mlog("comments %lld: page=%d next=%d more=%d", song_id,
         g_comment_page, g_comment_off, g_comment_more);
    lock_give(g_lock);
    post_ex(EV_COMMENTS_OK, offset > 0 ? 1 : 0, song_id, 0, generation, "");
}

static void job_user_detail(void)
{
    char url[256];
    char err[80] = "";

    snprintf(url, sizeof(url), API_BASE "/api/v1/user/detail/%lld", g_login_uid);
    if (fetch_json(url, err, sizeof(err)) < 0) return;
    json_walk(g_buf, (int)strlen(g_buf), user_cb, NULL);
    mlog("user: level=%d listen=%d", g_app.user_level, g_app.user_listen);
}


static void queue_job(int job, int arg, const char *query, const Song *song);
#include "comment_api.inc"
#include "search_categories.inc"

/* ------------------------------------------------------------------ */
/* 封面缩略图缓存（歌单网格用）                                          */
/* ------------------------------------------------------------------ */

#define THUMB_MAX 24
#define THUMB_W   112
#define THUMB_Q   12
#define THUMB_DISK_SLOTS 256
#define THUMB_BYTES (THUMB_W * THUMB_W * 2)

typedef struct {
    char url[560];
    unsigned short px[THUMB_W * THUMB_W];
    int state;          /* 0 空 / 1 排队 / 2 就绪 / 3 失败 */
    int stamp;
    unsigned fail_t;
    unsigned generation;
} Thumb;

static Thumb *g_thumbs; /* Owned by the reclaimable high-partition image cache. */
static int g_thumb_stamp;
typedef struct { char url[560]; int slot; unsigned generation; } ImageRequest;
static unsigned g_image_generation;
static ImageRequest g_image_batch[THUMB_Q]; /* 6816 bytes; reused for avatars. */
static ImageRequest g_thumb_q[THUMB_Q];
static int g_thumb_qn;
static ImageRequest g_av_q[6];          /* 头像队列（job_thumbs 一起处理） */
static int g_av_qn;

/* 封面磁盘缓存：固定 256 个槽（文件名只跟 hash 有关），命中直接读盘，
 * 重进歌单/翻回上一页就不用再走 WiFi。一个槽 25KB，总共最多 ~6.4MB。 */
static unsigned hash_url(const char *s)
{
    unsigned h = 2166136261u;

    while (s && *s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h ? h : 1;
}

static void thumb_disk_init(void)
{
    sceIoMkdir(APP_DIR "cache", 0777);
    sceIoMkdir(APP_DIR "cache/thumb", 0777);
}

static int thumb_disk_load(const char *url, unsigned short *px)
{
    char path[72];
    unsigned h = hash_url(url), stored = 0;
    FILE *fp;

    snprintf(path, sizeof(path), APP_DIR "cache/thumb/t%u.565", h % THUMB_DISK_SLOTS);
    io_lock();
    fp = fopen(path, "rb");
    if (!fp) { io_unlock(); return 0; }
    if (fread(&stored, 4, 1, fp) != 1 || stored != h) {
        fclose(fp);
        io_unlock();
        return 0;
    }
    if (fread(px, 2, THUMB_W * THUMB_W, fp) != (size_t)(THUMB_W * THUMB_W)) {
        fclose(fp);
        io_unlock();
        return 0;
    }
    fclose(fp);
    io_unlock();
    return 1;
}

static void thumb_disk_save(const char *url, const unsigned short *px)
{
    char path[72];
    unsigned h = hash_url(url);
    FILE *fp;
    int failed = 0;

    snprintf(path, sizeof(path), APP_DIR "cache/thumb/t%u.565", h % THUMB_DISK_SLOTS);
    io_lock();
    fp = fopen(path, "wb");
    if (fp) {
        int valid = fwrite(&h, 4, 1, fp) == 1;
        if (valid) valid = fwrite(px, 2, THUMB_W * THUMB_W, fp) == (size_t)(THUMB_W * THUMB_W);
        if (fclose(fp) != 0) valid = 0;
        if (!valid) {
            remove(path);
            failed = 1;
        }
    } else failed = 1;
    io_unlock();
    if (failed) mlog("thumb: cache write failed");
}

/* Use exactly the same key for startup reads and worker reads/writes. */
static void image_request_url(const char *source, int dim, char *url, size_t size)
{
    const char *base = !strncasecmp(source, "https://", 8) ? source + 8 : source;
    const char *q = strchr(base, '?');
    int bl = q ? (int)(q - base) : (int)strlen(base);
    if (strstr(base, "music.126.net/"))
        snprintf(url, size, "%s%.*s?imageView&thumbnail=%dx%d&type=jpg",
                 base != source ? "http://" : "", bl, base, dim, dim);
    else
        snprintf(url, size, "%s%.*s?param=%dy%d",
                 base != source ? "http://" : "", bl, base, dim, dim);
}

/* Called only while the API worker is stopped. Restore at most the first
 * twelve recommendation covers, once per cold slot, before network jobs.
 * No UI-frame disk access, JPEG staging buffer or extra thread is required. */
static void home_thumbs_restore(void)
{
    int restored = 0, checked = 0;
    if (!g_thumbs) return;
    for (int i = 0; i < g_home_cards.count && i < THUMB_Q; ++i) {
        const char *source = g_home_cards.items[i].cover;
        char url[620];
        int slot = -1, found = 0;
        if (!source[0]) continue;
        for (int j = 0; j < THUMB_MAX; ++j) {
            if (!strcmp(g_thumbs[j].url, source) && g_thumbs[j].state == 2) {
                found = 1;
                break;
            }
            if (!g_thumbs[j].url[0] && slot < 0) slot = j;
        }
        if (found || slot < 0) continue;
        image_request_url(source, THUMB_W, url, sizeof(url));
        ++checked;
        if (!thumb_disk_load(url, g_thumbs[slot].px)) continue;
        safe_copy(g_thumbs[slot].url, sizeof(g_thumbs[slot].url), source);
        g_thumbs[slot].stamp = ++g_thumb_stamp;
        g_thumbs[slot].generation = ++g_image_generation;
        g_thumbs[slot].state = 2;
        ++restored;
    }
    if (checked) mlog("home thumbs: restored %d/%d local covers before worker", restored, checked);
}

int api_thumb_slot(const char *url)
{
    int slot = -1, result = -1;
    unsigned now = sceKernelGetSystemTimeLow();
    if (!g_thumbs || !url || !url[0] || strlen(url) >= sizeof(g_thumbs[0].url) || !lock_take(g_lock)) return -1;
    for (int i = 0; i < THUMB_MAX; ++i) {
        if (!strcmp(g_thumbs[i].url, url)) {
            if (g_thumbs[i].state == 3 && (unsigned)(now - g_thumbs[i].fail_t) >= 30000000u) {
                slot = i;
                break;
            }
            g_thumbs[i].stamp = ++g_thumb_stamp;
            result = g_thumbs[i].state == 2 ? i : -1;
            lock_give(g_lock);
            return result;
        }
        if (!g_thumbs[i].url[0] && slot < 0) slot = i;
    }
    if (g_thumb_qn >= THUMB_Q) { lock_give(g_lock); return -1; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < THUMB_MAX; ++i)
            if (g_thumbs[i].stamp < g_thumbs[slot].stamp) slot = i;
    }
    g_thumbs[slot].state = 1;
    safe_copy(g_thumbs[slot].url, sizeof(g_thumbs[slot].url), url);
    g_thumbs[slot].stamp = ++g_thumb_stamp;
    g_thumbs[slot].generation = ++g_image_generation;
    ImageRequest *r = &g_thumb_q[g_thumb_qn++];
    safe_copy(r->url, sizeof(r->url), url);
    r->slot = slot;
    r->generation = g_thumbs[slot].generation;
    lock_give(g_lock);
    return -1;
}

const unsigned short *api_thumb_px(int slot)
{
    const unsigned short *px = NULL;
    if (!g_thumbs || slot < 0 || slot >= THUMB_MAX || !lock_take(g_lock)) return NULL;
    if (g_thumbs[slot].state == 2) px = g_thumbs[slot].px;
    lock_give(g_lock);
    return px;
}

void api_request_thumbs(void)
{
    int pending;
    if (!lock_take(g_lock)) return;
    pending = g_thumb_qn > 0 || g_av_qn > 0;
    lock_give(g_lock);
    if (pending) queue_job(JOB_THUMBS, 0, "", NULL);
}


/* ------------------------------------------------------------------ */
/* 评论头像缓存（48x48，10 个槽）                                        */
/* ------------------------------------------------------------------ */

#define AV_MAX 24
#define AV_W   48

typedef struct {
    char url[560];
    unsigned short px[AV_W * AV_W];
    int state;
    int stamp;
    unsigned fail_t;
    unsigned generation;
} Avatar;

static Avatar *g_avs;
static int g_av_stamp;

/* These 723KiB used to be permanent BSS even after api_free_buffers().
 * Keep them outside the fixed libc heap so MV can reclaim the partition. */
typedef struct { Thumb thumbs[THUMB_MAX]; Avatar avatars[AV_MAX]; } ImageCacheStorage;
static SceUID g_image_cache_block = -1;

/* Main-thread lifecycle only, before starting / after joining the API worker.
 * Pixel pointers returned to the UI expire at api_free_buffers(), like art. */
static void image_cache_alloc(void)
{
    ImageCacheStorage *storage;
    if (g_image_cache_block >= 0) return;
    g_image_cache_block = sceKernelAllocPartitionMemory(2, "melody_images",
                              PSP_SMEM_High, sizeof(ImageCacheStorage), NULL);
    if (g_image_cache_block < 0) return;
    storage = sceKernelGetBlockHeadAddr(g_image_cache_block);
    if (!storage) {
        sceKernelFreePartitionMemory(g_image_cache_block);
        g_image_cache_block = -1;
        return;
    }
    memset(storage, 0, sizeof(*storage));
    g_thumbs = storage->thumbs;
    g_avs = storage->avatars;
}

static void image_cache_release(void)
{
    /* A live worker may still own a batch or be writing decoded pixels. */
    if (g_thid >= 0) return;
    g_thumb_qn = g_av_qn = 0;
    ++g_image_generation;
    g_thumbs = NULL;
    g_avs = NULL;
    g_thumb_stamp = g_av_stamp = 0;
    if (g_image_cache_block >= 0) sceKernelFreePartitionMemory(g_image_cache_block);
    g_image_cache_block = -1;
}



int api_avatar_slot(const char *url)
{
    int slot = -1, result = -1;
    unsigned now = sceKernelGetSystemTimeLow();
    if (!g_avs || !url || !url[0] || strlen(url) >= sizeof(g_avs[0].url) || !lock_take(g_lock)) return -1;
    for (int i = 0; i < AV_MAX; ++i) {
        if (!strcmp(g_avs[i].url, url)) {
            if (g_avs[i].state == 3 && (unsigned)(now - g_avs[i].fail_t) >= 30000000u) {
                slot = i;
                break;
            }
            g_avs[i].stamp = ++g_av_stamp;
            result = g_avs[i].state == 2 ? i : -1;
            lock_give(g_lock);
            return result;
        }
        if (!g_avs[i].url[0] && slot < 0) slot = i;
    }
    if (g_av_qn >= 6) { lock_give(g_lock); return -1; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < AV_MAX; ++i)
            if (g_avs[i].stamp < g_avs[slot].stamp) slot = i;
    }
    g_avs[slot].state = 1;
    safe_copy(g_avs[slot].url, sizeof(g_avs[slot].url), url);
    g_avs[slot].stamp = ++g_av_stamp;
    g_avs[slot].generation = ++g_image_generation;
    ImageRequest *r = &g_av_q[g_av_qn++];
    safe_copy(r->url, sizeof(r->url), url);
    r->slot = slot;
    r->generation = g_avs[slot].generation;
    lock_give(g_lock);
    return -1;
}

const unsigned short *api_avatar_px(int slot)
{
    const unsigned short *px = NULL;
    if (!g_avs || slot < 0 || slot >= AV_MAX || !lock_take(g_lock)) return NULL;
    if (g_avs[slot].state == 2) px = g_avs[slot].px;
    lock_give(g_lock);
    return px;
}


/* Caller holds g_lock. A request may outlive an LRU slot reuse. */
static int image_request_current(const ImageRequest *r, int avatar)
{
    if (avatar) return g_avs && r->slot >= 0 && r->slot < AV_MAX &&
        g_avs[r->slot].state == 1 && g_avs[r->slot].generation == r->generation &&
        !strcmp(g_avs[r->slot].url, r->url);
    return g_thumbs && r->slot >= 0 && r->slot < THUMB_MAX &&
        g_thumbs[r->slot].state == 1 && g_thumbs[r->slot].generation == r->generation &&
        !strcmp(g_thumbs[r->slot].url, r->url);
}

/* Once a batch has left the UI queue, transient lock timeouts must not
 * discard ownership. Only shutdown may interrupt this worker-only wait. */
static int image_batch_lock(void)
{
    while (!g_quit) {
        if(net_offline()){sceKernelDelayThread(100000);continue;}
        if (lock_take(g_lock)) return 1;
        sceKernelDelayThread(1000);
    }
    return 0;
}

/* Caller holds g_lock. Finish the current HTTP only, then yield to input. */
static int image_interactive_waiting(void)
{
    if (g_play.job) return 1;
    for (int i = 0; i < g_jq_n; ++i) {
        int job = g_jq[(g_jq_head + i) % JOBQ_MAX].job;
        if (job == JOB_COMMENTS || job == JOB_COMMENT_REPLIES || job == JOB_SUGGEST) return 1;
    }
    return 0;
}

static void job_image_batch(int avatar)
{
    int n;
    if (!image_batch_lock()) return;
    n = avatar ? g_av_qn : g_thumb_qn;
    memcpy(g_image_batch, avatar ? g_av_q : g_thumb_q, (size_t)n * sizeof(ImageRequest));
    if (avatar) g_av_qn = 0;
    else g_thumb_qn = 0;
    lock_give(g_lock);
    for (int i = 0; i < n; ++i) {
        const ImageRequest *r = &g_image_batch[i];
        int dim = avatar ? AV_W : THUMB_W;
        int len = 0, w = 0, h = 0, ok = 0, current, deferred;
        char url[620], err[64] = "";
        /* Decode uses at most 112*112*4 bytes at the front of g_rgba.
         * Its unused tail stages RGB565, avoiding another 25KiB allocation. */
        unsigned short *pixels = g_rgba ?
            (unsigned short *)(g_rgba + ART_MAX * ART_MAX * 4 - THUMB_BYTES) : NULL;
        if (!image_batch_lock()) return;
        current = image_request_current(r, avatar);
        deferred = image_interactive_waiting();
        lock_give(g_lock);
        if (!current) continue;
        image_request_url(r->url, dim, url, sizeof(url));
        int disk_hit=!avatar&&pixels&&thumb_disk_load(url,pixels);
        if(disk_hit)ok=1;
        if (!ok&&!g_abort && !deferred && pixels && g_jpg) {
            if (http_get(url, (char *)g_jpg, JPEG_MAX, &len, 8000, err, sizeof(err)) >= 0 &&
                     !g_abort && len > 0 && len < JPEG_MAX &&
                     (http_get_expect() <= 0 || len >= http_get_expect()) &&
                     decode_cover_rgba(g_jpg, len, dim, &w, &h) == 0 &&
                     art_fit_rgba565(g_rgba, (size_t)w * h * 4, w, h,
                                     pixels, THUMB_BYTES, dim, dim) == 0) {
                ok = 1;
                if (!avatar) thumb_disk_save(url, pixels);
            }
        }
        if (!image_batch_lock()) return;
        if (image_request_current(r, avatar)) {
            if (avatar) {
                if (ok && !g_abort) memcpy(g_avs[r->slot].px, pixels, (size_t)dim * dim * 2);
                g_avs[r->slot].fail_t = sceKernelGetSystemTimeLow() - (deferred ? 30000000u : 0u);
                g_avs[r->slot].state = ok && !g_abort ? 2 : 3;
            } else {
                if (ok && (!g_abort||disk_hit)) memcpy(g_thumbs[r->slot].px, pixels, THUMB_BYTES);
                g_thumbs[r->slot].fail_t = sceKernelGetSystemTimeLow() - (deferred ? 30000000u : 0u);
                g_thumbs[r->slot].state = ok && (!g_abort||disk_hit) ? 2 : 3;
            }
        }
        lock_give(g_lock);
    }
}

static void job_thumbs(void)
{
    job_image_batch(0);
    job_image_batch(1);
}
/* 批量查可播性：把 data[i] 的 id/url 收起来，再按 id 回填 */
typedef struct {
    long long ids[64];
    int url_ok[64];
    int n;
} CheckCtx;

static void check_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    CheckCtx *c = (CheckCtx *)ud;
    const char *seg[6];
    int len[6];
    int n = json_split_path(path, seg, len, 6);
    int i;

    if (n != 3 || !seg_is(seg[0], len[0], "data")) return;

    i = atoi(seg[1]);
    if (i < 0 || i >= 64) return;
    if (i + 1 > c->n) c->n = i + 1;

    if (type == JSON_NUM && seg_is(seg[2], len[2], "id")) {
        c->ids[i] = num;
    } else if (type == JSON_STR && seg_is(seg[2], len[2], "url")) {
        if (str && str[0]) c->url_ok[i] = 1;
    }
}

/* 修复：ids 只有 700 字节，约 53 首就塞不下；以前超出部分全被标成
 * playable=2（变灰）。现在按 40 首一批分多次查，只回填真正查到的歌。 */
#define CHECK_BATCH 40

static void job_check(void)
{
    int total = g_pending.count;

    if (total <= 0) {
        post(EV_CHECK_DONE, 0, "");
        return;
    }

    for (int start = 0; start < total; start += CHECK_BATCH) {
        int end = start + CHECK_BATCH;
        char ids[700];
        char url[900];
        char err[80] = "";
        int o = 0;
        CheckCtx ctx;

        if (end > total) end = total;
        if (g_abort) break;      /* 播放/搜索插队：不再发下一批 */

        memset(&ctx, 0, sizeof(ctx));

        o += snprintf(ids + o, sizeof(ids) - (size_t)o, "%%5B");
        for (int i = start; i < end; ++i) {
            o += snprintf(ids + o, sizeof(ids) - (size_t)o, "%s%lld",
                          i > start ? "%2C" : "", g_pending.songs[i].id);
            if (o > (int)sizeof(ids) - 32) break;
        }
        snprintf(ids + o, sizeof(ids) - (size_t)o, "%%5D");

        snprintf(url, sizeof(url), API_BASE "/api/song/enhance/player/url?ids=%s&br=%d",
                 ids, api_quality_br());

        if (fetch(url, err, sizeof(err)) < 0) {
            if (g_abort) break;
            continue;            /* 这一批失败：这些歌保持 playable=0（未知） */
        }

        json_walk(g_buf, (int)strlen(g_buf), check_cb, &ctx);

        for (int i = start; i < end; ++i) {
            for (int j = 0; j < ctx.n; ++j) {
                if (ctx.ids[j] == g_pending.songs[i].id) {
                    g_pending.songs[i].playable = ctx.url_ok[j] ? 1 : 2;
                    break;
                }
            }
        }
    }

    /* 修复：被打断时也要发 EV_CHECK_DONE，否则 UI 的可播性检查状态清不掉 */
    post(EV_CHECK_DONE, 0, "");
}

static int g_br_downgrade;      /* 播放失败过（多半是 403），下次直接用 128k */

int api_quality_br(void)
{
    if (g_br_downgrade) return 128000;
    if (g_app.quality == 1) return 192000;
    if (g_app.quality == 2) return 320000;
    return 128000;
}

void api_note_play_fail(void)
{
    if (!g_br_downgrade) mlog("play: downgrade to 128k after failure");
    g_br_downgrade = 1;
}


#define CLOUD_LIKE_CAPACITY 4096
static long long cloud_like_ids[CLOUD_LIKE_CAPACITY];
static int cloud_like_n,cloud_like_complete,cloud_like_queued;
static unsigned cloud_like_account;
static void cloud_like_cb(void*ud,const char*path,int type,const char*str,long long number){
    (void)ud;(void)str;const char*seg[3];int len[3];if(type!=JSON_NUM||json_split_path(path,seg,len,3)!=2||!seg_is(seg[0],len[0],"ids"))return;
    int index=atoi(seg[1]);if(index<0)return;if(index>=CLOUD_LIKE_CAPACITY){cloud_like_complete=0;return;}
    cloud_like_ids[index]=number;if(index+1>cloud_like_n)cloud_like_n=index+1;
}
static void job_likes_refresh(void){
    unsigned account=api_account_generation();char url[192],err[80]={0};
    if(!g_logged_in)goto done;
    snprintf(url,sizeof(url),API_BASE "/api/song/like/get?uid=%lld",g_login_uid);
    if(fetch_json(url,err,sizeof(err))<0){mlog("likes: state refresh failed %s",err);goto done;}
    if(account!=api_account_generation())goto done;
    if(!lock_take(g_lock))goto done;
    cloud_like_n=0;cloud_like_complete=1;
    if(json_walk(g_buf,(int)strlen(g_buf),cloud_like_cb,NULL)<0)cloud_like_complete=0;
    /* Empty successful IDs are valid; a login/error response is not. */
    if(!strstr(g_buf,"\"ids\""))cloud_like_complete=0;
    cloud_like_account=account;if(cloud_like_complete)g_liked_total=cloud_like_n;cloud_like_queued=0;lock_give(g_lock);
    if(cloud_like_complete)liked_pl_save();
    mlog("likes: cloud status ids=%d complete=%d accountgen=%u",cloud_like_n,cloud_like_complete,account);return;
 done:if(lock_take(g_lock)){cloud_like_queued=0;lock_give(g_lock);}
}
int api_song_liked(long long id){
    int result=-1;if(!g_logged_in||id<=0||!lock_take(g_lock))return -1;
    if(cloud_like_account==g_account_generation){if(cloud_like_complete)result=0;for(int i=0;i<cloud_like_n;++i)if(cloud_like_ids[i]==id){result=1;break;}}
    lock_give(g_lock);return result;
}
void api_request_like_state(void){
    if(!g_logged_in||!lock_take(g_lock))return;
    int needed=cloud_like_account!=g_account_generation||!cloud_like_complete;
    if(!needed||cloud_like_queued){lock_give(g_lock);return;}
    cloud_like_queued=1;lock_give(g_lock);queue_job(JOB_LIKES_REFRESH,0,NULL,NULL);
}
void api_request_saved_playlists(void){queue_job(JOB_SAVED_PLAYLISTS,0,NULL,NULL);}

/* ---------------- 喜欢 / 收藏 ---------------- */

static char g_csrf[64];

static const char *csrf_token(void)
{
    const char *ck = net_get_cookie();
    const char *p;

    g_csrf[0] = 0;
    if (!ck) return g_csrf;
    p = strstr(ck, "__csrf=");
    if (!p) return g_csrf;
    p += 7;
    for (int i = 0; p[i] && p[i] != ';' && i < (int)sizeof(g_csrf) - 1; ++i) g_csrf[i] = p[i];
    return g_csrf;
}

/* 喜欢 / 收藏歌单都要带 cookie，而且要走 api 线程。

 * 以前这两个函数在 UI 线程直接 fetch()：和 api 线程共用 g_buf / g_abort，
 * 一次点赞会把正在跑的「取播放地址」写坏，还会把界面卡住几秒。
 * 现在统一投任务，结果用 EV_LIKE_OK 回来。 */
static void operation_code_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    (void)str;
    if (type == JSON_NUM && !strcmp(path, "code")) *(int *)ud = (int)num;
}

static void job_like(const char *q, int like)
{
    char url[320];
    char err[80] = "";
    long long id = atoll(q);
    unsigned int req = 0, account = 0;
    const char *bar = strchr(q, '|');
    int ok;
    if (bar) { req = (unsigned int)strtoul(bar + 1, NULL, 10); bar = strchr(bar + 1, '|'); }
    if (bar) account = (unsigned int)strtoul(bar + 1, NULL, 10);
    if (account != api_account_generation()) return;

    if (!g_logged_in) {
        post_ex(EV_LIKE_OK, -1, id, account, req, "先登录");
        return;
    }
    snprintf(url, sizeof(url), API_BASE "/api/song/like?id=%lld&like=%s&csrf_token=%s",
             id, like ? "true" : "false", csrf_token());
    if (fetch(url, err, sizeof(err)) < 0) {
        mlog("like %lld failed: %s", id, err);
        if (!g_abort && account == api_account_generation()) post_ex(EV_LIKE_OK, -1, id, account, req, "云端同步失败，喜欢状态未改变");
        return;
    }
    ok = 0;
    if (json_walk(g_buf, (int)strlen(g_buf), operation_code_cb, &ok) < 0) ok = 0;
    ok = ok == 200;
    if(ok&&account==api_account_generation()){
        if(lock_take(g_lock)){
            int at=-1;for(int i=0;i<cloud_like_n;++i)if(cloud_like_ids[i]==id){at=i;break;}
            if(like&&at<0&&cloud_like_n<CLOUD_LIKE_CAPACITY)cloud_like_ids[cloud_like_n++]=id;
            if(!like&&at>=0){memmove(cloud_like_ids+at,cloud_like_ids+at+1,(size_t)(cloud_like_n-at-1)*sizeof(long long));--cloud_like_n;}
            cloud_like_account=account;cloud_like_complete=0;lock_give(g_lock);
        }
        /* Force the next liked-list request to fetch updated membership/count. */
        g_liked_total+=like?1:-1;if(g_liked_total<0)g_liked_total=0;liked_pl_save();
    }
    mlog("like %lld -> %d %s", id, like, ok ? "ok" : "rejected");
    if (!g_abort && account == api_account_generation())
        post_ex(EV_LIKE_OK, ok ? like : -1, id, account, req,
                ok ? (like ? "云端已喜欢" : "云端已取消喜欢") : "网易云没有接受（没登录或权限不足）");
    if(ok&&!g_abort)job_likes_refresh();
}

static void job_subscribe(const char *q, int sub)
{
    char url[320];
    char err[80] = "";
    long long id = atoll(q);const char*bar=strchr(q,'|');unsigned account=bar?(unsigned)strtoul(bar+1,NULL,10):0;
    int ok;
    if(account!=api_account_generation())return;
    if (!g_logged_in) {
        post(EV_LIKE_OK, -1, "先登录");
        return;
    }
    snprintf(url,sizeof(url),API_BASE "/api/playlist/subscribe");
    char form[256];snprintf(form,sizeof(form),"id=%lld&t=%d&csrf_token=%s",id,sub?1:2,csrf_token());int response_bytes=0;
    if(http_post_form(url,form,g_buf,BUF_SIZE-1,&response_bytes,8000,err,sizeof(err))<0) {
        mlog("subscribe %lld failed: %s", id, err);
        if (!g_abort) post(EV_INFO,-1,"云端歌单同步失败，本地收藏保留");
        return;
    }
    ok = 0;
    if (json_walk(g_buf, (int)strlen(g_buf), operation_code_cb, &ok) < 0) ok = 0;
    ok = ok == 200;
    mlog("subscribe %lld -> %d %s", id, sub, ok ? "ok" : "rejected");
    if (!g_abort&&account==api_account_generation())
        post(EV_INFO, ok ? 100 : -1,
             ok ? (sub ? "云端已收藏歌单" : "云端已取消收藏") : "网易云没有接受（没登录或权限不足）");
}

unsigned int api_account_generation(void) {
    unsigned int n;
    if (!lock_take(g_lock)) return 0;
    n = g_account_generation; lock_give(g_lock); return n;
}
unsigned int api_like_generation(void) {
    unsigned int n;
    if (!lock_take(g_lock)) return 0;
    n = g_like_request_generation; lock_give(g_lock); return n;
}

void api_request_like(long long id, int like)
{
    char q[64];

    if (!g_logged_in) {
        post(EV_LIKE_OK, -1, "先登录");
        return;
    }
    if (!lock_take(g_lock)) return;
    ++g_like_request_generation;
    snprintf(q, sizeof(q), "%lld|%u|%u", id, g_like_request_generation, g_account_generation);
    lock_give(g_lock);
    queue_job(JOB_LIKE, like, q, NULL);
}

void api_request_subscribe(long long id, int sub)
{
    char q[32];

    if (!g_logged_in) {
        post(EV_LIKE_OK, -1, "先登录");
        return;
    }
    snprintf(q, sizeof(q), "%lld|%u", id,api_account_generation());
    queue_job(JOB_SUBSCRIBE, sub, q, NULL);
}

void api_note_play_ok(void)
{
    g_br_downgrade = 0;
}

static void job_play(int index, const Song *song)
{
    char err[80] = "";
    char alturl[900];
    char altsrc[24];
    UrlCtx ctx;
    int have;

    /* A newer play/search owns the result after any blocking lookup. */
    if (g_abort) return;
    have = cache_has(song->id);
    if (g_abort) return;

    /* 已经在缓存里：不联网，直接放 */
    if (have) {
        api_clear_art();
        if (audio_play_cached(song) > 0) {
            post(EV_PLAY_OK, index, "");
            if (load_art(song) == 0) post(EV_ART_OK, index, "");
            else if(!song->art[0]){char id[32];snprintf(id,sizeof(id),"%lld",song->id);job_local_id(id);}
        } else {
            post(EV_PLAY_FAIL, index, audio_error());
        }
        return;
    }

    /* 没版权/地区受限的歌：官方接口基本不给可用地址，直接走第三方通道，
     * 省掉一次必然失败的 8 秒等待；第三方也拿不到时才回退官方解析。 */
    if (song->playable == 2) {
        alturl[0] = 0;
        if (alt_resolve(song, alturl, sizeof(alturl), altsrc, sizeof(altsrc))) {
            if (g_abort) return;
            memset(&ctx, 0, sizeof(ctx));
            snprintf(ctx.url, sizeof(ctx.url), "%s", alturl);
            ctx.size = alt_last_size();
            have = 1;
            mlog("play: no-copyright -> alt source %s for %lld (size=%d)", altsrc, song->id, ctx.size);
            post(EV_INFO, 0, "无版权，已从第三方音源获取");
        } else {
            have = resolve_url(song, &ctx, err, sizeof(err));
            if (g_abort) return;
        }
    } else {
        have = resolve_url(song, &ctx, err, sizeof(err));
        if (g_abort) return;
    }

    if (have && ctx.trial) {
        /* VIP 歌只给 45 秒试听：先问备用源要完整版，要不到就放试听 */
        alturl[0] = 0;
        if (alt_resolve(song, alturl, sizeof(alturl), altsrc, sizeof(altsrc))) {
            if (g_abort) return;
            snprintf(ctx.url, sizeof(ctx.url), "%s", alturl);
            ctx.size = alt_last_size();
            ctx.trial = 0;
            mlog("play: trial -> alt source %s for %lld (size=%d)", altsrc, song->id, ctx.size);
            post(EV_INFO, 0, "VIP 仅试听，已切换备用音源");
        } else {
            if (g_abort) return;
            post(EV_INFO, 0, "VIP 歌只能试听 45 秒（设置里可开备用音源）");
        }
    } else if (!have) {
        /* 官方完全没地址：走备用音源 */
        alturl[0] = 0;
        if (alt_resolve(song, alturl, sizeof(alturl), altsrc, sizeof(altsrc))) {
            if (g_abort) return;
            memset(&ctx, 0, sizeof(ctx));
            snprintf(ctx.url, sizeof(ctx.url), "%s", alturl);
            ctx.size = alt_last_size();
            have = 1;
            mlog("play: alt source %s for %lld (size=%d)", altsrc, song->id, ctx.size);
            post(EV_INFO, 0, "已从备用音源获取");
        } else {
            if (g_abort) return;
            post(EV_PLAY_FAIL, index, "VIP / 无版权");
            return;
        }
    }

    /* 先探一下地址能不能下：VIP/无版权时服务端照样给 URL，但 CDN 直接 403，
     * 播放器就会一直转圈。探一次几百毫秒，能让界面立刻跳到下一首。 */
    {
        Http h;
        char perr[64];

        if (g_abort) return;
        if (http_open_range(&h, ctx.url, 0, 1023, 8000, perr, sizeof(perr)) < 0) {
            http_close(&h);
            if (g_abort) return;
            mlog("play: url rejected (%s) for %lld", perr, song->id);
            post(EV_PLAY_FAIL,index,strstr(perr,"403")?"片源拒绝访问或无权限":"网络连接失败，请重新联网后重试");
            return;
        }
        http_close(&h);
    }

    if (g_abort) return;
    api_clear_art();
    if (audio_play_stream(song, ctx.url, ctx.size) > 0) {
        post(EV_PLAY_OK, index, "");
        if (load_art(song) == 0) post(EV_ART_OK, index, "");
    } else {
        post(EV_PLAY_FAIL, index, "播放失败");
    }
}

static void lyric_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    (void)ud;
    (void)num;
    if (type == JSON_STR && !strcmp(path, "lrc.lyric")) parse_lyric(str);
}

static void job_export(const Song *song)
{
    int r = local_export(song);

    if (r == 0) post(EV_INFO, 0, "已导出到 音乐/MELODY");
    else if (r == -2) post(EV_INFO, 0, "记忆棒空间不够");
    else if (r == -1) post(EV_INFO, 0, "这首歌还没缓存");
    else post(EV_INFO, 0, "导出失败");
}

static void job_lyric(const Song *song)
{
    char url[256];
    char err[80] = "";

    g_lyric_word_count=0;
    g_lyric.count=0;g_lyric.translated_count=0;memset(g_lyric.translation,0,sizeof(g_lyric.translation));

    /* 先看本地缓存：离线也能看歌词 */
    {
        static char lbuf[8192];

        if (cache_lyric_read(song->id, lbuf, sizeof(lbuf)) > 0) {
            json_walk(lbuf, (int)strlen(lbuf), lyric_cb, NULL);
            json_walk(lbuf,(int)strlen(lbuf),yrc_cb,NULL);
            json_walk(lbuf,(int)strlen(lbuf),translation_cb,NULL);
            if (g_lyric.count > 0) {
                mlog("lyric from cache %d lines", g_lyric.count);
                post(EV_LYRIC_OK, 0, "");
                return;
            }
        }
    }

    snprintf(url, sizeof(url), API_BASE "/api/song/lyric?id=%lld&lv=-1&kv=-1&tv=-1&yv=-1", song->id);

    if (fetch(url, err, sizeof(err)) < 0) return;

    json_walk(g_buf, (int)strlen(g_buf), lyric_cb, NULL);
    json_walk(g_buf,(int)strlen(g_buf),yrc_cb,NULL);
    json_walk(g_buf,(int)strlen(g_buf),translation_cb,NULL);

    if (g_lyric.count > 0) cache_lyric_write(song->id, g_buf, (int)strlen(g_buf));

    post(EV_LYRIC_OK, 0, "");
}

static void job_art(const Song *song)
{
    if(net_offline())return;
    if (load_art(song) == 0) post(EV_ART_OK, 0, "");
}

static void job_download(const Song *song)
{
    char err[80] = "";
    UrlCtx ctx;
    int r;

    if (cache_has(song->id)||dl_has_song(song->id)) {
        if(!g_download_job_silent)post(EV_DL_FAIL, 0, "已缓存或已在下载队列");
        return;
    }

    /* 先用匿名身份取地址：带 cookie 拿到的是会话签名 URL，CDN 会 403 */
    if (!resolve_url(song, &ctx, err, sizeof(err)) || ctx.trial) {
        char alturl[900];
        char altsrc[24];

        if (alt_resolve(song, alturl, sizeof(alturl), altsrc, sizeof(altsrc))) {
            memset(&ctx, 0, sizeof(ctx));
            snprintf(ctx.url, sizeof(ctx.url), "%s", alturl);
            ctx.size = alt_last_size();
            mlog("download: alt source %s for %lld (size=%d)", altsrc, song->id, ctx.size);
        } else {
            post(EV_DL_FAIL, 0, ctx.trial ? "VIP 歌只能试听，没法下载" : "VIP / 无版权，没法下载");
            return;
        }
    }

    if(g_abort||g_download_job_epoch!=g_download_epoch)return;
    r = dl_enqueue(song, ctx.url, ctx.size);
    if(g_download_job_silent)return;
    if (r == -2) post(EV_DL_FAIL, 0, "已经在缓存里了");
    else if (r == -3) post(EV_DL_FAIL, 0, "已经在下载列表里");
    else if (r < 0) post(EV_DL_FAIL, 0, "下载队列满了");
    else post(EV_DL_STARTED, r, song->name);
}

/* ------------------------------------------------------------------ */
/* 本地歌曲：按「歌名 + 歌手」去网易搜一次，补封面和歌词                    */
/* ------------------------------------------------------------------ */

static int g_local_req;        /* 每次播放本地歌 +1，晚到的结果直接丢掉 */

/* 宽松匹配：忽略空格/标点/大小写，允许「歌名里带 Live 版」这类差异 */
static int name_like(const char *a, const char *b)
{
    char na[80], nb[80];
    int i = 0;

    if (!a || !b) return 0;
    for (; a && *a && i < (int)sizeof(na) - 1; ++a) {
        unsigned char c = (unsigned char)*a;

        if (c == ' ' || c == '\t' || c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '-' || c == '_' || c == '.' || c == '\'' || c == '"')
            continue;
        na[i++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    na[i] = 0;
    i = 0;
    for (; b && *b && i < (int)sizeof(nb) - 1; ++b) {
        unsigned char c = (unsigned char)*b;

        if (c == ' ' || c == '\t' || c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '-' || c == '_' || c == '.' || c == '\'' || c == '"')
            continue;
        nb[i++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    nb[i] = 0;
    if (!na[0] || !nb[0]) return 0;
    return strstr(na, nb) != NULL || strstr(nb, na) != NULL;
}

/* 归一化：小写、去空白/ASCII 标点，用于更严格的比较（UTF-8 字节原样保留） */
static void norm_text(const char *in, char *out, int outlen)
{
    int o = 0;

    if (outlen <= 0) return;
    while (in && *in && o < outlen - 1) {
        unsigned char c = (unsigned char)*in++;

        if (c == ' ' || c == '\t' || c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '-' || c == '_' || c == '.' || c == '\'' || c == '"' ||
            c == '!' || c == '?' || c == ',' || c == ':' || c == ';' || c == '/')
            continue;
        out[o++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[o] = 0;
}

/* 歌手列表里任意一个 token 相同就算匹配（"陈奕迅/张学友" 之类） */
static int artist_overlap(const char *a, const char *b)
{
    char na[120], nb[120];
    char *p;

    norm_text(a, na, sizeof(na));
    norm_text(b, nb, sizeof(nb));
    if (!na[0] || !nb[0]) return 0;
    if (strstr(na, nb) || strstr(nb, na)) return 1;

    /* 逐个 / & , 拆开比；token 至少 2 字节，避免单字母误匹配 */
    p = na;
    while (*p) {
        char *q = p;
        char tok[64];
        int n;

        while (*q && *q != '/' && *q != '&' && *q != ',') q++;
        n = (int)(q - p);
        if (n > 0 && n < (int)sizeof(tok)) {
            memcpy(tok, p, (size_t)n);
            tok[n] = 0;
            if (n >= 2 && strstr(nb, tok)) return 1;
        }
        p = (*q) ? q + 1 : q;
    }
    return 0;
}

/* 版本标记：0 普通 / 1 Live / 2 伴奏 / 3 翻唱 / 4 Remix / 5 纯音乐 */
static int version_tag(const char *s)
{
    if (!s) return 0;
    if (strstr(s, "Live") || strstr(s, "live") || strstr(s, "现场")) return 1;
    if (strstr(s, "伴奏") || strstr(s, "instrumental")) return 2;
    if (strstr(s, "翻唱") || strstr(s, "cover") || strstr(s, "Cover")) return 3;
    if (strstr(s, "remix") || strstr(s, "Remix") || strstr(s, "DJ")) return 4;
    if (strstr(s, "纯音乐") || strstr(s, "off vocal")) return 5;
    return 0;
}

/* 给候选打分；低于阈值宁可不匹配，也不要放错封面/歌词 */
static int score_candidate(const Song *c, const char *name, const char *artist, int dur_ms)
{
    int score = 0;
    char nn[90], cn[90];

    norm_text(name, nn, sizeof(nn));
    norm_text(c->name, cn, sizeof(cn));
    if (!nn[0] || !cn[0]) return -999;
    if (!strcmp(nn, cn)) score += 100;
    else if (strstr(cn, nn) || strstr(nn, cn)) score += 60;
    else if (!name_like(name, c->name)) return -999;     /* 连模糊都不沾边：直接丢 */
    else score += 20;

    if (artist && artist[0]) {
        if (artist_overlap(artist, c->artist)) score += 50;
        else score -= 40;                                 /* 歌手对不上，很可能是翻唱/别的歌 */
    }

    if (dur_ms > 30000) {
        int d = c->dur_ms - dur_ms;

        if (d < 0) d = -d;
        if (d <= 5000) score += 40;
        else if (d <= 10000) score += 20;
        else if (d > 25000) score -= 80;
    }

    {
        int lv = version_tag(name), cv = version_tag(c->name);

        if (lv == cv) score += 15;
        else if (cv != 0) score -= 30;                    /* 本地是原版、候选是 Live/伴奏 */
    }
    return score;
}

/* 文件名就是网易云 id（缓存文件都长这样）：一次详情请求直接拿封面+歌词，
 * 不再按歌名搜索 —— 缓存文件的 ID3 里没有标题，搜数字只会搜到乱七八糟的歌。 */
static void job_local_id(const char *q)
{
    long long id = atoll(q);
    char url[300];
    char err[80] = "";
    ListCtx ctx;
    Song found;

    if (id <= 0) return;
    snprintf(url, sizeof(url),
             API_BASE "/api/v3/song/detail?c=%%5B%%7B%%22id%%22%%3A%lld%%7D%%5D", id);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        mlog("local id: detail failed %s", err);
        return;
    }
    memset(&g_pending, 0, sizeof(g_pending));
    ctx.list = &g_pending;
    ctx.last_idx = -1;
    ctx.base = 0;
    json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);
    if (g_pending.count <= 0) return;
    found = g_pending.songs[0];
    mlog("local id: %lld name=[%s] art=%d", id, found.name, found.art[0] != 0);
    if (found.art[0] && load_art(&found) == 0) post(EV_ART_OK, 0, "");
    job_lyric(&found);
}

static void job_local_meta(const char *q)
{
    char name[80], artist[80], want[170], enc[520], url[700];
    char err[80] = "";
    char *bar1;
    const char *rest;
    int gen, dur_ms = 0;
    ListCtx ctx;
    Song found;
    int pick = -1, best = 0;

    /* q = "gen|dur|name|artist" */
    bar1 = strchr(q, '|');
    if (!bar1) return;
    gen = atoi(q);
    rest = bar1 + 1;
    {
        const char *bar2 = strchr(rest, '|');

        if (!bar2) return;
        dur_ms = atoi(rest);
        rest = bar2 + 1;
    }
    {
        const char *bar3 = strchr(rest, '|');

        if (!bar3) return;
        {
            int n = (int)(bar3 - rest);

            if (n >= (int)sizeof(name)) n = (int)sizeof(name) - 1;
            memcpy(name, rest, (size_t)n);
            name[n] = 0;
        }
        safe_copy(artist, sizeof(artist), bar3 + 1);
    }

    if (artist[0] && strstr(artist, "Unknown") == NULL &&
        strstr(artist, "unknown") == NULL)
        snprintf(want, sizeof(want), "%s %s", name, artist);
    else
        snprintf(want, sizeof(want), "%s", name);
    net_url_encode(want, enc, sizeof(enc));
    snprintf(url, sizeof(url), API_BASE "/api/search/get?s=%s&type=1&limit=10", enc);
    if (fetch_json(url, err, sizeof(err)) < 0) {
        mlog("local meta: search failed %s", err);
        return;
    }

    memset(&g_pending, 0, sizeof(g_pending));
    ctx.list = &g_pending;
    ctx.last_idx = -1;
    ctx.base = 0;
    json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);
    if (gen != g_local_req) return;      /* 用户已经切歌了 */

    /* 打分挑一个「同一首录音」，不再无脑取第一条 */
    for (int i = 0; i < g_pending.count && i < 10; ++i) {
        int sc = score_candidate(&g_pending.songs[i], name, artist, dur_ms);

        if (sc > best) { best = sc; pick = i; }
    }
    mlog("local meta: [%s / %s dur=%d] candidates=%d best=%d pick=%d",
         name, artist, dur_ms, g_pending.count, best, pick);
    if (pick < 0 || best < 100) {
        post(EV_INFO, 0, "本地歌曲：没找到可靠匹配");
        return;
    }
    found = g_pending.songs[pick];

    /* 搜索接口不给封面 URL，再拿一次详情（响应 ~2KB，很快） */
    snprintf(url, sizeof(url),
             API_BASE "/api/v3/song/detail?c=%%5B%%7B%%22id%%22%%3A%lld%%7D%%5D", found.id);
    if (fetch_json(url, err, sizeof(err)) >= 0) {
        memset(&g_pending, 0, sizeof(g_pending));
        ctx.list = &g_pending;
        ctx.last_idx = -1;
        ctx.base = 0;
        json_walk(g_buf, (int)strlen(g_buf), list_cb, &ctx);
        if (g_pending.count > 0) {
            const Song *d = &g_pending.songs[0];

            if (d->art[0]) safe_copy(found.art, sizeof(found.art), d->art);
            if (d->album[0]) safe_copy(found.album, sizeof(found.album), d->album);
            if (d->dur_ms > 0) found.dur_ms = d->dur_ms;
        }
    }
    if (gen != g_local_req) return;

    /* 详情里的时长再核对一次：差太多就不补了，宁缺毋滥 */
    if (dur_ms > 30000 && found.dur_ms > 0) {
        int d = found.dur_ms - dur_ms;

        if (d < 0) d = -d;
        if (d > 12000) {
            mlog("local meta: reject duration %d vs %d", found.dur_ms, dur_ms);
            return;
        }
    }

    mlog("local meta: matched id=%lld score=%d art=%d", found.id, best, found.art[0] != 0);
    if (found.art[0] && load_art(&found) == 0) post(EV_ART_OK, 0, "");
    job_lyric(&found);
}

void api_request_local_meta(const char *name, const char *artist, int dur_ms)
{
    char q[320];

    if (!name || !name[0]) return;
    if (strchr(name, '?')) return;      /* GBK 文件名被替换过，搜了也没用 */
    if (strlen(name) > 70 || (artist && strlen(artist) > 70)) return;
    g_local_req++;
    snprintf(q, sizeof(q), "%d|%d|%s|%s", g_local_req, dur_ms > 0 ? dur_ms : 0,
             name, artist ? artist : "");
    queue_job(JOB_LOCAL_META, 0, q, NULL);
}

void api_request_local_id(long long id)
{
    char q[32];

    if (id <= 0) return;
    snprintf(q, sizeof(q), "%lld", id);
    queue_job(JOB_LOCAL_ID, 0, q, NULL);
}

/* ------------------------------------------------------------------ */
/* 在线更新                                                            */
/* ------------------------------------------------------------------ */

#define UPDATE_CONF APP_DIR "update.txt"
#define UPDATE_NEW  APP_DIR "EBOOT.NEW"

static volatile int g_upd_state;      /* 0 idle 1 checking 2 latest 3 available 4 dl 5 done 6 err */
static volatile int g_upd_progress;
static volatile int g_upd_abort;
static volatile int g_upd_pending;    /* EBOOT.NEW 已下载，等下次启动替换 */
static char g_upd_ver[24];
static char g_upd_url[1000];
static char g_upd_msg[120];
static long long g_upd_size;
static char g_upd_log[4096];          /* 更新日志（清单第 4 行起） */

/* 支持 3.3 / 3.3.1 / 3.4.0.2，忽略 v 前缀和 -beta 之类后缀；
 * 用 strtol 而不是 sscanf：后者不检查返回值，也没法处理 4 段。 */
static void upd_ver_parse(const char *s, int *v, int n)
{
    for (int i = 0; i < n; ++i) v[i] = 0;
    if (*s == 'v' || *s == 'V') ++s;
    for (int i = 0; i < n && *s; ++i) {
        char *e = NULL;

        v[i] = (int)strtol(s, &e, 10);
        if (!e || *e != '.') break;
        s = e + 1;
    }
}

static int upd_ver_cmp(const char *a, const char *b)
{
    int va[4], vb[4];

    upd_ver_parse(a, va, 4);
    upd_ver_parse(b, vb, 4);
    for (int i = 0; i < 4; ++i)
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return 0;
}

static void upd_trim(char *s)
{
    int n = (int)strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
}

/* update.txt 第一行非空、非 # 的内容就是版本清单地址 */
static int upd_read_conf(char *out, int n)
{
    char buf[1200];
    char *p;
    int fd, r;

    out[0] = 0;
    char conf_path[768],root[512];const char*conf=UPDATE_CONF;
    if(getcwd(root,sizeof(root))){snprintf(conf_path,sizeof(conf_path),"%s/update.txt",root);conf=conf_path;}
    fd = sceIoOpen(conf, PSP_O_RDONLY, 0);
    if(fd<0){snprintf(out,(size_t)n,"http://melody-1320953126.cos.ap-nanjing.myqcloud.com/melody/version.txt");return 0;}
    r = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (r <= 0) return -1;
    buf[r] = 0;

    p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');

        if (nl) *nl = 0;
        upd_trim(p);
        if (p[0] && p[0] != '#') {
            snprintf(out, (size_t)n, "%s", p);
            return 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return -1;
}

static void job_update_check(void)
{
    char url[1000];
    char buf[4096];
    char err[96] = "";
    char *lines[32] = { 0 };
    char *p;
    int len = 0, nl = 0, i;

    g_upd_state = 1;
    g_upd_progress = 0;
    g_upd_msg[0] = 0;
    g_upd_ver[0] = 0;
    g_upd_url[0] = 0;
    g_upd_log[0] = 0;
    g_upd_size = 0;

    if (upd_read_conf(url, sizeof(url)) < 0) {
        g_upd_state = 6;
        snprintf(g_upd_msg, sizeof(g_upd_msg), "未配置 update.txt");
        mlog("update: update.txt missing");
        return;
    }
    if (strncmp(url, "http://", 7) != 0) {
        g_upd_state = 6;
        snprintf(g_upd_msg, sizeof(g_upd_msg), "更新地址必须是 http://");
        return;
    }

    mlog("update: check %s", url);
    if (http_get(url, buf, sizeof(buf) - 1, &len, 10000, err, sizeof(err)) < 0 || len <= 0) {
        g_upd_state = 6;
        snprintf(g_upd_msg, sizeof(g_upd_msg), "获取版本失败: %s", err[0] ? err : "网络错误");
        mlog("update: get failed %s", err);
        return;
    }
    buf[len] = 0;

    p = buf;
    while (p && *p && nl < 32) {
        char *e = strchr(p, '\n');

        if (e) *e = 0;
        upd_trim(p);
        if (*p && *p != '#') lines[nl++] = p;
        p = e ? e + 1 : NULL;
    }
    if (nl < 2) {
        g_upd_state = 6;
        snprintf(g_upd_msg, sizeof(g_upd_msg), "版本文件格式不对");
        return;
    }

    snprintf(g_upd_ver, sizeof(g_upd_ver), "%s", lines[0]);
    snprintf(g_upd_url, sizeof(g_upd_url), "%s", lines[1]);
    if (nl >= 3) g_upd_size = atoll(lines[2]);

    /* 第 4 行起：key=value 是元数据（size=… / crc32=… / min=…），
     * 其余行拼成更新日志，给启动弹窗显示。 */
    {
        int o = 0;

        for (i = 3; i < nl; ++i) {
            char *s = lines[i];
            char *eq = strchr(s, '=');
            int meta = 0;

            if (eq && eq != s) {
                int k = 1;

                for (char *c = s; c < eq; ++c)
                    if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) {
                        k = 0;
                        break;
                    }
                meta = k;
                if (meta && !strncmp(s, "size=", 5) && g_upd_size <= 0)
                    g_upd_size = atoll(s + 5);
            }
            if (meta) continue;
            {
                int l = (int)strlen(s);

                if (l > 0 && o + l + 2 < (int)sizeof(g_upd_log)) {
                    memcpy(g_upd_log + o, s, l);
                    o += l;
                    g_upd_log[o++] = '\n';
                }
            }
        }
        g_upd_log[o] = 0;
    }

    if (strncmp(g_upd_url, "http://", 7) != 0) {
        g_upd_state = 6;
        snprintf(g_upd_msg, sizeof(g_upd_msg), "下载地址必须是 http://");
        return;
    }

    if (upd_ver_cmp(g_upd_ver, APP_VER) > 0) g_upd_state = 3;
    else g_upd_state = 2;

    mlog("update: local=%s remote=%s state=%d size=%lld", APP_VER, g_upd_ver,
         (int)g_upd_state, g_upd_size);
}

static void job_update_download(void)
{
    static unsigned char ubuf[65536];
    Http h;
    char err[96] = "";
    long long total, done = 0,received=0;
    unsigned used=0,read_calls=0,write_calls=0,write_us=0;
    unsigned transfer_start=sceKernelGetSystemTimeLow(),last_rx=transfer_start;
    int fd = -1;
    int r = 0;

    if(g_upd_state!=7)return;
    if(g_upd_abort){g_upd_state=3;return;}
    g_upd_state = 4;
    g_upd_progress = 0;
    g_upd_msg[0] = 0;

    if (http_open(&h, g_upd_url, 0, 15000, err, sizeof(err)) < 0) {
        g_upd_state = 6;
        snprintf(g_upd_msg, sizeof(g_upd_msg), "下载连接失败: %s", err[0] ? err : "网络错误");
        return;
    }
    total = h.content_length;
    if (h.status != 200 || total <= 0 || (g_upd_size > 0 && total != g_upd_size)) {
        http_close(&h);
        g_upd_state = 6;
        snprintf(g_upd_msg, sizeof(g_upd_msg), "服务器没给大小，无法校验");
        mlog("update: no length, refuse");
        return;
    }

    {
        SceIoStat meta_st;
        if (sceIoGetstat(APP_DIR "update.meta", &meta_st) >= 0 &&
            sceIoRemove(APP_DIR "update.meta") < 0) {
            http_close(&h); g_upd_state = 6;
            snprintf(g_upd_msg, sizeof(g_upd_msg), "旧更新记录清理失败"); return;
        }
    }
    fd = sceIoOpen(UPDATE_NEW, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        http_close(&h);
        g_upd_state = 6;
        snprintf(g_upd_msg,sizeof(g_upd_msg),"无法写入 EBOOT.NEW (%08X)",fd);
        mlog("update: create EBOOT.NEW failed result=0x%08X",fd);
        return;
    }

    for (;;) {
        if(g_upd_abort){r=-2;break;}
        r=http_read(&h,(char*)ubuf+used,(int)(sizeof(ubuf)-used),1000,&g_upd_abort,0,err,sizeof(err));
        if(r>0){
            read_calls++;last_rx=sceKernelGetSystemTimeLow();
            if((unsigned)r>sizeof(ubuf)-used||received+r>total){r=-2;break;}
            used+=(unsigned)r;received+=r;
            if(used==sizeof(ubuf)||received==total){
                unsigned begin=sceKernelGetSystemTimeLow();write_calls++;
                if(io_write_complete(fd,ubuf,(int)used)<0){r=-2;break;}
                write_us+=sceKernelGetSystemTimeLow()-begin;done+=used;used=0;
                sceKernelDelayThread(500); /* A bounded chunk yields input/UI even on fast local HTTP. */
            }
            int pct=(int)(received*100/total);g_upd_progress=pct>99?99:pct;
        }else if(r==0){break;}
        else if(r==-1){
            if((unsigned)(sceKernelGetSystemTimeLow()-last_rx)>=20000000u){snprintf(err,sizeof(err),"HTTP body idle20s");r=-2;break;}
        }else break;
    }
    unsigned elapsed=sceKernelGetSystemTimeLow()-transfer_start;
    mlog("update transfer: received=%lld written=%lld rate=%dKiB/s reads=%u writes=%u write_time=%ums elapsed=%ums",received,done,elapsed?(int)(received*1000000/elapsed/1024):0,read_calls,write_calls,write_us/1000,elapsed/1000);

    http_close(&h);
    if (sceIoClose(fd) < 0) r = -2;
    if (sceIoSync("ms0:", 0) < 0) r = -2;

    if (g_upd_abort) {
        sceIoRemove(UPDATE_NEW);
        g_upd_state = 0;
        g_upd_progress = 0;
        snprintf(g_upd_msg, sizeof(g_upd_msg), "已取消下载");
        return;
    }
    if (r < 0 || done != total || done < 1000000 || (g_upd_size > 0 && done != g_upd_size)) {
        sceIoRemove(UPDATE_NEW);
        g_upd_state = 6;
        snprintf(g_upd_msg, sizeof(g_upd_msg), "下载不完整 %lld/%lld", done, g_upd_size);
        mlog("update: incomplete %lld/%lld %s", done, g_upd_size, err);
        return;
    }

    {
        unsigned char magic[40];
        int mfd = sceIoOpen(UPDATE_NEW, PSP_O_RDONLY, 0);
        int mret = (mfd >= 0) ? sceIoRead(mfd, magic, sizeof(magic)) : -1;

        if (mfd >= 0 && sceIoClose(mfd) < 0) mret = -1;
        if (mret != sizeof(magic) || !update_pbp_valid(magic, sizeof(magic), (unsigned long long)total)) {
            sceIoRemove(UPDATE_NEW);
            g_upd_state = 6;
            snprintf(g_upd_msg, sizeof(g_upd_msg), "下载文件不是 PBP");
            return;
        }
    }

    /* 记下期望大小：启动替换前要复核（防御残缺/被截断的 EBOOT.NEW） */
    {
        int mfd = sceIoOpen(APP_DIR "update.meta",
                            PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        char mb[32];
        int mn = snprintf(mb, sizeof(mb), "%lld", total);
        int mok = mfd >= 0 ? io_write_complete(mfd, mb, mn) : -1;
        if (mfd >= 0 && sceIoClose(mfd) < 0) mok = -1;
        if (sceIoSync("ms0:", 0) < 0) mok = -1;
        if (mok < 0 || g_upd_abort) {
            sceIoRemove(UPDATE_NEW);
            sceIoRemove(APP_DIR "update.meta");
            g_upd_state = 6;
            snprintf(g_upd_msg, sizeof(g_upd_msg), "更新校验记录保存失败");
            return;
        }
    }
    g_upd_progress = 100;
    g_upd_state = 5;
    mlog("update: downloaded %lld bytes -> EBOOT.NEW", done);
}

/* ------------------------------------------------------------------ */
/* MV 详情                                                             */
/* ------------------------------------------------------------------ */

static MvInfo g_mv;
static MvInfo g_mv_downloading;
static char mv_urls[3][900];
static int mv_selected_resolution;
static volatile int mv_download_only;
static volatile int g_mv_dl_state;    /* 0 idle 1 downloading 2 done 3 fail */
static volatile int g_mv_dl_pct;
static volatile int g_mv_dl_abort;
static volatile long long g_mv_dl_total;

static void mv_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    const char *seg[6];
    int len[6];
    int n = json_split_path(path, seg, len, 6);

    (void)ud;
    if (n < 2 || !seg_is(seg[0], len[0], "data")) return;

    if (n == 2) {
        if (type == JSON_NUM && seg_is(seg[1], len[1], "duration"))
            g_mv.dur_ms = (int)num;
        else if (type == JSON_STR && str) {
            if (seg_is(seg[1], len[1], "name"))
                safe_copy(g_mv.name, sizeof(g_mv.name), str);
            else if (seg_is(seg[1], len[1], "artistName"))
                safe_copy(g_mv.artist, sizeof(g_mv.artist), str);
            else if (seg_is(seg[1], len[1], "cover"))
                safe_copy(g_mv.cover, sizeof(g_mv.cover), str);
        }
        return;
    }

    if(n==3&&seg_is(seg[1],len[1],"brs")&&type==JSON_STR&&str){
        const char *rates[]={"240","360","480"};
        for(int i=0;i<3;++i)if(seg_is(seg[2],len[2],rates[i]))safe_copy(mv_urls[i],sizeof(mv_urls[i]),str);
    }
}

static void job_mv_detail(int mvid)
{
    char url[160];
    char err[80] = "";
    int len = 0;

    memset(&g_mv, 0, sizeof(g_mv));memset(mv_urls,0,sizeof(mv_urls));
    g_mv.id = mvid;
    g_mv.state = 1;

    snprintf(url, sizeof(url), API_BASE "/api/mv/detail?id=%d", mvid);
    if (http_get(url, g_buf, BUF_SIZE - 1, &len, 8000, err, sizeof(err)) < 0 || len <= 0) {
        g_mv.state = 3;
        mlog("mv %d: detail failed %s", mvid, err);
        return;
    }
    g_buf[len] = 0;
    json_walk(g_buf, len, mv_cb, NULL);
    int requested=mv_resolution(),effective=mv_download_resolution(),target=effective==480?2:effective==360?1:0,chosen=-1;
    for(int i=target;i>=0;--i)if(mv_urls[i][0]){chosen=i;break;}
    if(chosen<0)for(int i=0;i<3;++i)if(mv_urls[i][0]){chosen=i;break;}
    if(chosen>=0){safe_copy(g_mv.url,sizeof(g_mv.url),mv_urls[chosen]);mv_selected_resolution=chosen==0?240:chosen==1?360:480;}
    mlog("mv selection: requested=%d selected=%dp mode=%d",requested,mv_selected_resolution,mv_decoder_mode());
    g_mv.state = g_mv.url[0] ? 2 : 3;
    mlog("mv %d: name=[%s] artist=[%s] %dms url=%s", mvid, g_mv.name, g_mv.artist,
         g_mv.dur_ms, g_mv.url[0] ? "yes" : "no");
}

static void mv_job_step(const char*tag){
 int fd=sceIoOpen(APP_DIR "mv-connect.txt",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND,0777);
 if(fd<0)return;
 unsigned n=0;while(tag[n])n++;
 sceIoWrite(fd,tag,n);sceIoWrite(fd,"\n",1);sceIoClose(fd);
}

static void job_mv_download(int mvid)
{
    static unsigned char buf[16384];
    Http h;
    char path[160], part[160], err[80] = "";
    long long total = 0, done = 0;
    int fd = -1;
    int r = 0;
    int stalls = 0;
    int last_pct = -1;
    int attempt;
    int close_result;

    mv_job_step("api-download-job-entered");
    mlog("mv dl job %d", mvid);

    /* 先清掉上一轮的进度/状态：这次就算卡在"取详情"阶段就失败，
     * 下载页也不会继续挂着旧百分比（用户看到"失败"却又显示 80%）。 */
    g_mv_stream_job = 0;
    if(g_mv_dl_abort){g_mv_dl_state=3;return;}
    g_mv_dl_state = 1;
    g_mv_dl_pct = 0;
    g_mv_dl_total = 0;

    mv_job_step("before-video-url");
    if (g_mv.id != mvid || g_mv.state != 2 || !g_mv.url[0] || (mv_download_resolution()!=mv_selected_resolution))
        job_mv_detail(mvid);
    mv_job_step("after-video-url");
    if (g_mv_dl_abort || g_mv.state != 2 || !g_mv.url[0]) {
        g_mv_dl_state = 3;
        return;
    }

    snprintf(path, sizeof(path), APP_DIR "cache/mv_%d_240.mp4", mvid);
    snprintf(part, sizeof(part), "%s.part", path);

    g_mv_downloading=g_mv;
    /* 边下边播（实验）：下载交给 mv.c 的独立线程，API 线程只负责解析 URL。
     * 这样 mv_start() 里的 api_shutdown() 不会把下载掐掉。 */
    if (mv_download_only||mv_stream_enabled()) {
        mv_job_step("before-download-source-start");
        if (mv_stream_start(g_mv.url, path) == 0) {
            mv_stream_duration(g_mv.dur_ms);
            mv_job_step("after-download-source-start");
            g_mv_stream_job = 1;
            if(g_mv_dl_abort){mv_stream_abort();g_mv_dl_state=3;return;}
            g_mv_dl_total = 0;
            g_mv_dl_pct = 0;
            g_mv_dl_state = 1;
            mlog("mv dl %d: streaming to %s", mvid, path);
            return;
        }
        mlog("mv dl %d: stream start failed, fall back to sync download", mvid);
    }

    if(g_mv_dl_abort){g_mv_dl_state=3;return;}
    g_mv_dl_state = 1;
    g_mv_dl_pct = 0;
    g_mv_dl_total = 0;

    /* 网络断了/被重置时不再"删掉半截重新下"：.part 就是已落盘的字节数，
     * 下一轮用 Range 从断点续传；最多 4 轮（1+3 次重试），间隔递增。
     * 之前一次掉线就 sceIoRemove(part) + failed，8.7MB 下到 70% 也白费。 */
    for (attempt = 0; attempt < 4; ++attempt) {
        SceIoStat st;
        long long have = 0;
        int resumed = 0;

        if (g_mv_dl_abort) break;

        if (sceIoGetstat(part, &st) >= 0 && st.st_size > 0) have = st.st_size;
        if (total > 0 && have > total) have = total;
        if (total > 0 && have >= total) { done = total; break; }

        if (have > 0) {
            if (http_open_range(&h, g_mv.url, have, -1, 15000, err, sizeof(err)) < 0) {
                mlog("mv dl %d: resume open failed (%s), attempt %d", mvid, err, attempt + 1);
                goto retry;
            }
            resumed = (h.status == 206 && h.range_start == have);
            if (!resumed) {
                /* 服务器不支持 Range（回了 200 整包）：只能从头来 */
                mlog("mv dl %d: no range (status=%d start=%lld), restart",
                     mvid, h.status, h.range_start);
                http_close(&h);
                sceIoRemove(part);
                have = 0;
            }
        }
        if (!resumed) {
            if (http_open_bulk(&h, g_mv.url, 15000, err, sizeof(err)) < 0) {
                mlog("mv dl %d: open failed (%s), attempt %d", mvid, err, attempt + 1);
                goto retry;
            }
        }
        if (total <= 0) {
            total = have + h.content_length;
            if (total <= 0) {
                http_close(&h);
                mlog("mv dl %d: no content-length", mvid);
                goto retry;
            }
            g_mv_dl_total = total;
        }

        fd = sceIoOpen(part, PSP_O_WRONLY | PSP_O_CREAT |
                            (have > 0 ? PSP_O_APPEND : PSP_O_TRUNC), 0777);
        if (fd < 0) {
            http_close(&h);
            g_mv_dl_state = 3;
            mlog("mv dl %d: cannot write %s", mvid, part);
            return;
        }

        done = have;
        r = 0;
        stalls = 0;
        last_pct = total > 0 ? (int)(done * 100 / total) : 0;
        if (done > 0) mlog("mv dl %d: resume at %lld/%lld", mvid, done, total);

        for (;;) {
            if (g_mv_dl_abort) { r = -2; break; }
            r = http_read(&h, (char *)buf, sizeof(buf), 1000, &g_mv_dl_abort, 0,
                          err, sizeof(err));
            if (r > 0) {
                if (sceIoWrite(fd, buf, r) != r) { r = -3; break; }
                done += r;
                if (done > total) break; /* Reject a body larger than its declared size. */
                stalls = 0;
                g_mv_dl_pct = (int)(done * 100 / total);
                if (g_mv_dl_pct > 99) g_mv_dl_pct = 99;
                if (g_mv_dl_pct / 10 != last_pct / 10) {
                    last_pct = g_mv_dl_pct;
                    mlog("mv dl %d: %d%% (%lld/%lld)", mvid, g_mv_dl_pct, done, total);
                }
                /* api 线程优先级高于主线程；大文件连续 recv/write 会把 UI 饿死。
                 * 每个 16KB 让出 1ms，代价不到 1 秒，换界面不冻。 */
                sceKernelDelayThread(1000);
            } else if (r == 0) {
                break;
            } else if (r == -1) {
                /* PSP 网络偶发超时/断流时不能原地忙等：高优先级 api 线程
                 * 会饿死主线程，表现就是整机死机。 */
                if (++stalls > 8) break;
                sceKernelDelayThread(20 * 1000);
            } else {
                break;
            }
        }

        http_close(&h);
        close_result = sceIoClose(fd);
        fd = -1;

        if (g_mv_dl_abort) {
            sceIoRemove(part);
            g_mv_dl_state = 0;
            mlog("mv dl %d: cancelled done=%lld/%lld", mvid, done, total);
            return;
        }
        if (r == -3 || close_result < 0) {
            sceIoRemove(part);
            g_mv_dl_state = 3;
            mlog("mv dl %d: file failed read=%d close=%d done=%lld/%lld",
                 mvid, r, close_result, done, total);
            return;
        }
        /* Exact bytes alone do not turn a transport error into clean EOF.
         * An invalid full part must not be accepted on the next retry. */
        if (done > total || (done == total && r != 0)) {
            sceIoRemove(part);
            g_mv_dl_state = 3;
            mlog("mv dl %d: invalid completion read=%d done=%lld/%lld", mvid, r, done, total);
            return;
        }
        if (total > 0 && done == total && r == 0) break;

        mlog("mv dl %d: interrupted at %lld/%lld (attempt %d)", mvid, done, total,
             attempt + 1);
    retry:
        if (attempt < 3) {
            for (int i = 0; i < (attempt + 1) * 10; ++i) {
                if (g_mv_dl_abort) break;
                sceKernelDelayThread(100 * 1000);
            }
        }
    }

    if (g_mv_dl_abort) {
        g_mv_dl_state = 0;
        return;
    }
    if (total <= 0 || done < total) {
        /* 半截 .part 留着：下次再点下载会从断点续传 */
        g_mv_dl_state = 3;
        mlog("mv dl %d: failed done=%lld/%lld (part kept for resume)", mvid, done, total);
        return;
    }

    sceIoRemove(path);
    if (sceIoRename(part, path) < 0) {
        sceIoRemove(part);
        g_mv_dl_state = 3;
        mlog("mv dl %d: rename failed", mvid);
        return;
    }
    g_mv_dl_pct = 100;
    {
        char meta[160];

        snprintf(meta, sizeof(meta), APP_DIR "cache/mv_%d_240.txt", mvid);
        int tf = sceIoOpen(meta, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);

        if (tf >= 0) {
            char mb[220];
            int mn = snprintf(mb, sizeof(mb), "%s\n%s\n%d\n",
                              g_mv.name, g_mv.artist, g_mv.dur_ms);

            sceIoWrite(tf, mb, mn);
            sceIoClose(tf);
        }
    }
    /* 记进 MV 列表索引（下载页列表只读索引，不再扫目录） */
    mv_index_add(mvid, (int)done);
    /* 11MB 刚写完时 FAT 缓存还没落盘，马上去扫目录会看不到新文件，
     * 甚至把 ms0: 驱动卡死。先 sync 再宣布完成。 */
    mlog("mv dl %d: sync ms0", mvid);
    sceIoSync("ms0:", 0);
    g_mv_dl_state = 2;
    mlog("mv dl %d: ok %lld bytes -> %s", mvid, done, path);
}

/* ------------------------------------------------------------------ */
/* 线程                                                                */
/* ------------------------------------------------------------------ */

/* A bulk update must not inherit the thumbnail/JSON worker's low UI priority.
 * Restore it on every result; receive waits and bounded write yields keep UI live. */
static void job_update_download_foreground(void)
{
    int previous=sceKernelGetThreadCurrentPriority();
    int boosted=previous>0x1c&&sceKernelChangeThreadPriority(0,0x1c)>=0;
    mlog("update foreground: previous_priority=%d active_priority=%d screen=%d",previous,boosted?0x1c:previous,g_app.screen);
    job_update_download();
    if(boosted&&sceKernelChangeThreadPriority(0,previous)<0)mlog("update foreground: priority restore failed");
}

static int api_bind_directory(void)
{
 char root[512];if(!getcwd(root,sizeof(root)))return -1;
 return sceIoChdir(root);
}

static int worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int directory_result=api_bind_directory();
    if(directory_result<0)mlog("api: bind file directory failed result=0x%08X",directory_result);

    while (!g_quit) {
        if(net_offline()){g_busy=0;sceKernelDelayThread(100000);continue;}
        int job = JOB_NONE;
        int arg = 0;
        char query[128];
        Song song;

        LOCK(g_lock, 1, NULL);
        if (g_play.job) {
            job = g_play.job;
            arg = g_play.arg;
            safe_copy(query, sizeof(query), g_play.query);
            song = g_play.song;
            g_play.job = JOB_NONE;
        } else if (g_jq_n > 0) {
            job = g_jq[g_jq_head].job;
            arg = g_jq[g_jq_head].arg;
            safe_copy(query, sizeof(query), g_jq[g_jq_head].query);
            song = g_jq[g_jq_head].song;
            g_jq_head = (g_jq_head + 1) % JOBQ_MAX;
            g_jq_n--;
        }
        if(job==JOB_NONE&&g_download_request_n>0&&dl_queue_count()<DL_QUEUE_MAX){job=JOB_DOWNLOAD;song=g_download_requests[0];g_download_job_silent=g_download_silent[0];arg=-77;}
        else g_download_job_silent=0;
        g_download_job_epoch=g_download_epoch;
        /* Preview never occupies the foreground FIFO or bypasses downloads. */
        if(job==JOB_NONE&&g_download_request_n==0&&g_daily_preview_pending){
            if(g_logged_in&&g_daily_preview_request_account==g_account_generation){
                job=JOB_DAILY_PREVIEW;arg=(int)g_daily_preview_request_account;
                g_daily_preview_inflight=1;
            }
            g_daily_preview_pending=0;
        }
        g_abort = 0;
        g_busy = (g_play.job || g_jq_n) ? 1 : 0;
        UNLOCK(g_lock, 1);

        if(job==JOB_NONE&&update_gallery_pending()){update_gallery_fetch();continue;}
        if (job == JOB_NONE) {
            sceKernelDelayThread(20 * 1000);
            continue;
        }

        switch (job) {
        case JOB_SKIN_REFRESH:skin_catalog_refresh();g_skin_request_busy=0;break;
        case JOB_SKIN_INSTALL:{int result=skin_catalog_install(query);g_skin_request_busy=0;if(result==0)post(EV_SKIN_INSTALLED,0,query);else post(EV_INFO,-1,skin_catalog_error());break;}
        case JOB_SKIN_PREVIEW:skin_catalog_fetch_preview(query);g_skin_request_busy=0;break;
        case JOB_PROBE:    job_probe(); break;
        case JOB_CONNECT:  job_connect(arg); break;
        case JOB_CHART:    job_chart(arg); break;
        case JOB_SEARCH:   job_search(query); break;
        case JOB_SEARCH_CATEGORY:job_search_category(query,arg);break;
        case JOB_SEARCH_OPEN:job_search_open(query,arg);break;
        case JOB_SUGGEST:  job_suggest(query); break;
        case JOB_QR_NEW:   job_qr_new((unsigned int)arg); break;
        case JOB_QR_POLL:  job_qr_poll((unsigned int)arg); break;
        case JOB_ACCOUNT:  job_account(); break;
        case JOB_DAILY:    job_daily(); break;
        case JOB_DAILY_PREVIEW: job_daily_preview((unsigned)arg); break;
        case JOB_REC_LISTS:
            cards_fetch(API_BASE "/api/personalized/playlist?limit=24&offset=0", "result", 0,
                        query[0] ? query : "推荐歌单", EV_CARDS_OK);
            break;
        case JOB_MY_LISTS: {
            char u[320];

            snprintf(u, sizeof(u), API_BASE "/api/user/playlist?uid=%lld&limit=24", g_login_uid);
            cards_fetch(u, "playlist", 1, "我的歌单", EV_CARDS_OK);
            break;
        }
        case JOB_TOPLISTS:
            cards_fetch(API_BASE "/api/toplist", "list", 2, "排行榜", EV_CARDS_OK);
            break;
        case JOB_ARTIST_TOP:
            cards_fetch(API_BASE "/api/toplist/artist?type=1", "artists", 3, "歌手榜", EV_CARDS_OK);
            break;
        case JOB_CAT_LISTS: {
            char u[360];
            char enc[80];

            net_url_encode(query, enc, sizeof(enc));
            snprintf(u, sizeof(u), API_BASE "/api/playlist/list?cat=%s&limit=24&offset=0", enc);
            cards_fetch(u, "playlists", 0, query, EV_CARDS_OK);
            break;
        }
        case JOB_PLAYLIST: job_playlist(atoll(query)); break;
        case JOB_PLAYLIST_MORE: job_playlist_more(query); break;
        case JOB_LIKED:    job_liked(); break;
        case JOB_LIKED_TOTAL: job_liked_total(); break;
        case JOB_PLAYED:   job_played(); break;
        case JOB_ARTIST_SONGS: {
            char nm[48];
            const char *bar = strchr(query, '|');

            snprintf(nm, sizeof(nm), "%s", bar ? bar + 1 : "歌手");
            job_artist_songs(atoll(query), nm);
            break;
        }
        case JOB_COMMENT_COUNT:job_comment_count(query,(unsigned)arg);break;
        case JOB_COMMENT_REPLIES: {
            const char *bar = strchr(query, '|');
            job_comment_replies(atoll(query), bar ? atoll(bar + 1) : 0, (unsigned int)arg);
            break;
        }
        case JOB_COMMENTS: {
            const char *bar = strchr(query, '|');

            job_comments(atoll(query), bar ? atoi(bar + 1) : 0, (unsigned int)arg);
            break;
        }
        case JOB_USER_DETAIL: job_user_detail(); break;
        case JOB_THUMBS:   job_thumbs(); break;
        case JOB_CHECK:    job_check(); break;
        case JOB_PLAY:     job_play(arg, &song); break;
        case JOB_LYRIC:    job_lyric(&song); break;
        case JOB_EXPORT:   job_export(&song); break;
        case JOB_ART:      job_art(&song); break;
        case JOB_LOCAL_META: job_local_meta(query); break;
        case JOB_LOCAL_ID:   job_local_id(query); break;
        case JOB_LIKE:     job_like(query, arg); break;
        case JOB_SUBSCRIBE: job_subscribe(query, arg); break;
        case JOB_DOWNLOAD:
            job_download(&song);
            if(arg==-77&&!g_abort&&lock_take(g_lock)){
                for(int i=0;i<g_download_request_n;++i)if(g_download_requests[i].id==song.id){for(int j=i+1;j<g_download_request_n;++j){g_download_requests[j-1]=g_download_requests[j];g_download_silent[j-1]=g_download_silent[j];}g_download_request_n--;break;}lock_give(g_lock);
            }break;
        case JOB_LIKES_REFRESH: job_likes_refresh(); break;
        case JOB_SAVED_PLAYLISTS:
            memset(&g_cards,0,sizeof(g_cards));g_cards.count=store_playlist_snapshot(g_cards.items,MAX_CARDS);safe_copy(g_cards.title,sizeof(g_cards.title),"收藏的歌单");post(EV_CARDS_OK,0,g_cards.title);break;
        case JOB_ANNOUNCEMENT_CHECK: if(arg)community_announcement_manual_check();else community_announcement_check(); break;
        case JOB_ANNOUNCEMENT_DISMISS: community_announcement_dismiss(arg); break;
        case JOB_UPDATE_CHECK: job_update_check(); break;
        case JOB_UPDATE_DL:    job_update_download_foreground(); break;
        case JOB_MV_DETAIL:    job_mv_detail(arg); break;
        case JOB_MV_DL:        job_mv_download(arg); break;
        default: break;
        }

        LOCK(g_lock, 1, NULL);
        g_busy = 0;
        UNLOCK(g_lock, 1);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int api_chart_count(void) { return (int)(sizeof(g_charts) / sizeof(g_charts[0])); }

const Chart *api_chart(int index)
{
    if (index < 0 || index >= api_chart_count()) return NULL;
    return &g_charts[index];
}

static void job_fill(JobItem *it, int job, int arg, const char *query, const Song *song)
{
    it->job = job;
    it->arg = arg;
    safe_copy(it->query, sizeof(it->query), query);
    if (song) it->song = *song;
}

static void queue_job(int job, int arg, const char *query, const Song *song)
{
    if(net_offline())return;
    LOCK(g_lock, 1, NULL);

    if (job == JOB_CONNECT || job == JOB_PLAY || job == JOB_SEARCH || job==JOB_SEARCH_CATEGORY || job==JOB_SEARCH_OPEN) {
        /* 用户刚点了播放/搜索：丢掉排队的榜单/检查任务，打断正在跑的请求，
         * 别让「点了新歌还在响老歌」「搜了半天没反应」发生。下载任务保留。 */
        int keep = 0;

        job_fill(&g_play, job, arg, query, song);
        g_abort = 1;

        /* 修复：保留的下载任务要压到 head 开始的连续位置，并且 head 不动。
         * 以前把任务搬到 (head+keep)%JOBQ_MAX 却把 head 归零，任务被孤立；
         * slot 0 还留着已经作废的旧任务，之后可能被重放。 */
        for (int i = 0; i < g_jq_n; ++i) {
            int k = (g_jq_head + i) % JOBQ_MAX;
            if (g_jq[k].job == JOB_DOWNLOAD || g_jq[k].job==JOB_UPDATE_DL) {
                if (keep != i) g_jq[(g_jq_head + keep) % JOBQ_MAX] = g_jq[k];
                keep++;
            }else if(g_jq[k].job==JOB_SKIN_REFRESH||g_jq[k].job==JOB_SKIN_INSTALL||g_jq[k].job==JOB_SKIN_PREVIEW){g_skin_request_busy=0;
            } else if (g_jq[k].job == JOB_MV_DL) {
                /* A discarded queued MV must not keep admission busy. */
                g_mv_dl_abort = 1;
                g_mv_dl_state = 3;
            }
        }
        g_jq_tail = (g_jq_head + keep) % JOBQ_MAX;
        g_jq_n = keep;
        /* 丢弃/搬空的槽位清零，旧任务绝不会被再执行一次 */
        for (int i = keep; i < JOBQ_MAX; ++i)
            memset(&g_jq[(g_jq_head + i) % JOBQ_MAX], 0, sizeof(g_jq[0]));

        g_busy = 1;
        UNLOCK(g_lock, 1);
        return;
    }

    /* 联想只保留尚未执行的最新输入，避免逐字请求占满 FIFO。 */
    if (job == JOB_SUGGEST || job == JOB_COMMENT_REPLIES) {
        for (int i = 0; i < g_jq_n; ++i) {
            int k = (g_jq_head + i) % JOBQ_MAX;
            if (g_jq[k].job == job) {
                job_fill(&g_jq[k], job, arg, query, song);
                UNLOCK(g_lock, 1);
                return;
            }
        }
    }

    /* 榜单/可播性检查/封面批量抓取按参数去重；搜索和下载每次都算新任务 */
    if (job == JOB_CHART || job == JOB_CHECK || job == JOB_THUMBS) {
        for (int i = 0; i < g_jq_n; ++i) {
            int k = (g_jq_head + i) % JOBQ_MAX;
            if (g_jq[k].job == job && g_jq[k].arg == arg) {
                UNLOCK(g_lock, 1);
                return;
            }
        }
    }

    if (g_jq_n >= JOBQ_MAX) {
        mlog("api: job queue full, drop job %d", job);
        if (job == JOB_COMMENT_REPLIES && (unsigned int)arg == g_reply_generation) { g_reply_loading = 0; g_reply_failed = 1; }
        if(job==JOB_MV_DL)g_mv_dl_state=3;
        if(job==JOB_COMMENT_COUNT)comment_count_queue_failed(atoll(query));
        if(job==JOB_SKIN_REFRESH||job==JOB_SKIN_INSTALL||job==JOB_SKIN_PREVIEW){g_skin_request_busy=0;post(EV_INFO,-1,"请求队列满，请稍后重试皮肤下载");}
        if(job==JOB_UPDATE_DL){g_upd_state=6;snprintf(g_upd_msg,sizeof(g_upd_msg),"请求队列已满，请稍后重试");}
        UNLOCK(g_lock, 1);
        return;
    }

    if(job==JOB_UPDATE_DL){g_jq_head=(g_jq_head+JOBQ_MAX-1)%JOBQ_MAX;job_fill(&g_jq[g_jq_head],job,arg,query,song);g_jq_n++;g_busy=1;UNLOCK(g_lock,1);return;}
    job_fill(&g_jq[g_jq_tail], job, arg, query, song);
    g_jq_tail = (g_jq_tail + 1) % JOBQ_MAX;
    g_jq_n++;
    g_busy = 1;

    UNLOCK(g_lock, 1);
}

void api_request_chart(int chart_index)
{
    queue_job(JOB_CHART, chart_index, NULL, NULL);
}

void api_request_probe(void)
{
    queue_job(JOB_PROBE, 0, NULL, NULL);
}

void api_request_connect(int profile)
{
    queue_job(JOB_CONNECT, profile, NULL, NULL);
}

int api_request_skin(int operation,const char*id){
 if(operation<0||operation>2)return -1;if(!lock_take(g_lock))return -1;if(g_skin_request_busy){lock_give(g_lock);return -1;}g_skin_request_busy=1;lock_give(g_lock);
 queue_job(operation==0?JOB_SKIN_REFRESH:operation==1?JOB_SKIN_INSTALL:JOB_SKIN_PREVIEW,0,id?id:"",NULL);return 0;
}
int api_skin_busy(void){return g_skin_request_busy;}

void api_request_search(const char *query)
{
    g_search_append = 0;
    g_search_off = 0;
    queue_job(JOB_SEARCH, 0, query, NULL);
}

void api_request_search_more(void)
{
    if (g_search_total <= 0 || g_search_off >= g_search_total) return;
    if (!g_search_q[0]) return;
    g_search_append = 1;
    queue_job(JOB_SEARCH, 0, g_search_q, NULL);
}

int api_list_total(void) { return g_list_total_hint > 0 ? g_list_total_hint : g_search_total; }

int api_playlist_count(void) { return g_pl_id_n; }

int api_liked_total(void) { return g_liked_total; }

void api_request_suggest(const char *query)
{
    if (!query || !query[0]) return;
    queue_job(JOB_SUGGEST, 0, query, NULL);
}

int api_suggest_count(void) { return g_sug_n; }
/* ---------------- 登录相关 ---------------- */

void api_request_qr_new(void)
{
    if (!lock_take(g_lock)) return;
    unsigned int generation = ++g_qr_generation;
    g_qr_account_generation = g_account_generation;
    g_qr_cookie[0] = g_qr_key[0] = 0;
    lock_give(g_lock);
    queue_job(JOB_QR_NEW, (int)generation, "", NULL);
}

void api_request_qr_poll(void)
{
    if (!lock_take(g_lock)) return;
    unsigned int generation = g_qr_generation;
    lock_give(g_lock);
    queue_job(JOB_QR_POLL, (int)generation, "", NULL);
}

void api_request_account(void)
{
    queue_job(JOB_ACCOUNT, 0, "", NULL);
}

int api_qr_state(void) { return g_qr_state; }
const char *api_qr_key(void) { return g_qr_key; }
const char *api_qr_url(void) { return g_qr_url; }

const unsigned char *api_qr_matrix(int *dim)
{
    if (g_qr_dim <= 0) return NULL;
    if (dim) *dim = g_qr_dim;
    return g_qr_mat;
}


/* ---------------- 云端内容 ---------------- */

void api_request_daily(void) { queue_job(JOB_DAILY, 0, "", NULL); }
void api_request_recommend_lists(void) { queue_job(JOB_REC_LISTS, 0, "", NULL); }

/* 社区页：和推荐歌单同一份数据，换个标题（分类接口的封面 URL 太长，解析会错位，先不用） */
void api_request_square(void) { api_request_discovery(-1); }
void api_request_my_lists(void) { queue_job(JOB_MY_LISTS, 0, "", NULL); }
void api_request_toplists(void) { queue_job(JOB_TOPLISTS, 0, "", NULL); }
void api_request_artist_top(void) { queue_job(JOB_ARTIST_TOP, 0, "", NULL); }
void api_request_cat_lists(const char *cat) { queue_job(JOB_CAT_LISTS, 0, cat ? cat : "", NULL); }
void api_request_liked(void) {if(lock_take(g_lock)){cloud_like_complete=0;lock_give(g_lock);}api_request_like_state();queue_job(JOB_LIKED,0,"",NULL);}

void api_request_liked_total(void)
{
    static unsigned last;
    unsigned now;

    if (!g_logged_in) return;
    now = (unsigned)sceKernelGetSystemTimeLow();
    if (last && now - last < 60000000u) return;   /* 60 秒内不重复请求 */
    last = now;
    queue_job(JOB_LIKED_TOTAL, 0, "", NULL);
}
void api_request_played(void) { queue_job(JOB_PLAYED, 0, "", NULL); }
void api_request_user_detail(void)
{
    static unsigned last;
    unsigned now = (unsigned)sceKernelGetSystemTimeLow();

    /* 离线时头像永远为空，界面每帧都会来请求；不节流会把 8 格队列塞满，
     * 连带播放/列表的请求全被丢掉（日志里成片的 "job queue full"）。 */
    if (last && now - last < 5000000) return;
    last = now;
    queue_job(JOB_USER_DETAIL, 0, "", NULL);
}

void api_request_playlist_more(int offset)
{
    char q[64];
    long long id = g_cur_playlist;

    snprintf(q, sizeof(q), "%lld|%d", id, offset);
    queue_job(JOB_PLAYLIST_MORE, 0, q, NULL);
}

void api_request_playlist(long long id)
{
    g_cur_playlist = id;
    char q[32];

    snprintf(q, sizeof(q), "%lld", id);
    queue_job(JOB_PLAYLIST, 0, q, NULL);
}

void api_request_artist_songs(long long id, const char *name)
{
    char q[96];

    snprintf(q, sizeof(q), "%lld|%s", id, name ? name : "");
    queue_job(JOB_ARTIST_SONGS, 0, q, NULL);
}

void api_request_comments(long long song_id, int offset)
{
    char q[48];

    if (offset < 0) offset = 0;
    snprintf(q, sizeof(q), "%lld|%d", song_id, offset);
    if (!lock_take(g_lock)) return;
    if (g_comment_requested_song != song_id || offset == 0)
        safe_copy(g_comment_requested_name, sizeof(g_comment_requested_name), g_app.now.id == song_id ? g_app.now.name : "歌曲评论");
    if (offset < 0) offset = 0;
    if (offset == 0 || song_id != g_comment_requested_song) {
        g_comment_more = 0;
        g_comment_norm_n = 0;
    }
    g_comment_requested_song = song_id;
    ++g_comment_request_gen;
    unsigned int generation = g_comment_request_gen;
    lock_give(g_lock);
    queue_job(JOB_COMMENTS, (int)generation, q, NULL);
}

/* Queue only while idle: a scrolling frame never displaces foreground work. */
void api_request_comments_more(void)
{
    char q[48];
    unsigned int generation;
    if (!lock_take(g_lock)) return;
    if (g_busy || g_comment_norm_n < 0 || !g_comment_more ||
        g_comment_song != g_comment_requested_song || g_comments.count >= MAX_COMMENTS) {
        lock_give(g_lock);
        return;
    }
    g_comment_norm_n = -1;
    generation = g_comment_request_gen;
    snprintf(q, sizeof(q), "%lld|%d", g_comment_song, g_comment_off);
    lock_give(g_lock);
    queue_job(JOB_COMMENTS, (int)generation, q, NULL);
}

int api_comments_has_more(void)
{
    return g_comment_requested_song == g_comment_song &&
           g_comments.count < MAX_COMMENTS && g_comment_more;
}
int api_comments_loading_more(void) { return g_comment_norm_n < 0; }

const CardList *api_cards(void) { return &g_cards; }
const CardList *api_home_cards(void) { return &g_home_cards; }

/* Discovery snapshot ownership. */
/* Filtered discovery lives on the existing API heap, not resident module BSS.
 * Home recommendations already have their own stable metadata owner. */
static CardList *g_square_snapshot;
static int g_discovery_category = -1;
static void discovery_cards_publish(void)
{
    if (g_square_snapshot) *g_square_snapshot = g_cards;
}
const CardList *api_discovery_cards(void)
{
    return g_discovery_category < 0 || !g_square_snapshot ? &g_home_cards : g_square_snapshot;
}
void api_request_discovery(int category)
{
    static const char *categories[] = { "华语", "流行", "摇滚", "电子", "民谣", "说唱", "轻音乐", "古典" };
    if (category < -1 || category > 7) return;
    if (category >= 0 && !g_square_snapshot) {
        g_square_snapshot = (CardList *)calloc(1, sizeof(CardList));
        if (!g_square_snapshot) { post(EV_LIST_FAIL, 0, "发现页内存不足"); return; }
    }
    g_discovery_category = category;
    if (category >= 0) api_request_cat_lists(categories[category]);
    else if (g_home_cards.count > 0) post(EV_CARDS_OK, 0, g_home_cards.title);
    else api_request_recommend_lists();
}
/* End discovery snapshot ownership. */

const CommentList *api_comments(void) { return g_comment_requested_song == g_comment_song ? &g_comments : &g_comments_empty; }

int api_comment_total(void) { return g_comment_requested_song == g_comment_song ? g_comment_total : 0; }
int api_comment_offset(void) { return g_comment_off; }
int api_comment_has_more(void) { return api_comments_has_more(); }
int api_comment_page(void) { return g_comment_page; }
const char *api_user_avatar(void) { return g_user_avatar; }
int api_logged_in(void) { return g_logged_in; }
const char *api_login_name(void) { return g_login_name; }
long long api_login_uid(void) { return g_login_uid; }
int api_login_vip(void) { return g_login_vip; }

const char *api_suggest(int i)
{
    if (i < 0 || i >= g_sug_n) return "";
    return g_sug[i];
}

void api_request_check(void)
{
    queue_job(JOB_CHECK, 0, NULL, NULL);
}

void api_request_play(int index, const Song *song)
{
    dl_stream_stop();
    queue_job(JOB_PLAY, index, NULL, song);
    if (song && song->mv > 0) api_request_mv_detail(song->mv);
}

void api_request_mv_detail(int mvid)
{
    if (mvid <= 0) return;
    if (g_mv.id == mvid && (g_mv.state == 1 || g_mv.state == 2)) return;
    queue_job(JOB_MV_DETAIL, mvid, NULL, NULL);
}

static int request_mv_download(int mvid,int save_only)
{
    mlog("mv req %d state=%d", mvid, g_mv_dl_state);
    if (mvid <= 0) return -1;
    /* Cancel/error is visible immediately, but the old writer must retire
     * before another job can touch the same .part pathname. */
    if (mv_stream_worker_active()) return -2;
    int effective=mv_download_effective_state(g_mv_dl_state,g_mv_stream_job,mv_stream_active(),mv_stream_done(),mv_stream_error(),mv_stream_cancelled());
    if (effective == 1) return -2;
    memset(&g_mv_downloading,0,sizeof(g_mv_downloading));g_mv_downloading.id=mvid;
    if(g_mv.id==mvid&&g_mv.state==2)g_mv_downloading=g_mv;
    mv_download_only=save_only;g_mv_stream_job=0;g_mv_dl_state=1;g_mv_dl_abort=0;
    queue_job(JOB_MV_DL, mvid, NULL, NULL);
    return 0;
}

void api_request_mv_download(int mvid){request_mv_download(mvid,0);}
int api_request_mv_save(int mvid){return request_mv_download(mvid,1);}

void api_mv_download_abort(void)
{
    LOCK(g_lock, 1, NULL);
    g_mv_dl_abort = 1;
    int keep = 0;
    int removed = 0;
    for (int i = 0; i < g_jq_n; ++i) {
        int k = (g_jq_head + i) % JOBQ_MAX;
        if (g_jq[k].job == JOB_MV_DL) {
            removed = 1;
            continue;
        }
        if (keep != i) g_jq[(g_jq_head + keep) % JOBQ_MAX] = g_jq[k];
        keep++;
    }
    if (removed) {
        for (int i = keep; i < g_jq_n; ++i)
            memset(&g_jq[(g_jq_head + i) % JOBQ_MAX], 0, sizeof(g_jq[0]));
        g_jq_n = keep;
        g_jq_tail = (g_jq_head + keep) % JOBQ_MAX;
        g_mv_dl_state = 3;
    }
    /* Dequeued jobs retain their abort flag until they actually retire. */
    if (g_mv_stream_job) mv_stream_abort();
    UNLOCK(g_lock, 1);
}
int api_mv_download_state(void)
{
    return mv_download_effective_state(g_mv_dl_state,g_mv_stream_job,mv_stream_active(),mv_stream_done(),mv_stream_error(),mv_stream_cancelled());
}

/* Bytes actually written for the MV being fetched. The UI must show this number
 * directly: deriving it from a percent (total*pct/100) made the row read
 * "11.5/11.5MB" while the same row's bar was at 33%. */
long long api_mv_download_bytes(void)
{
    if (g_mv_stream_job && mv_stream_total() > 0) {
        long long a = mv_stream_available();

        if (a < 0) a = 0;
        if (a > mv_stream_total()) a = mv_stream_total();
        return a;
    }
    if (g_mv_dl_total > 0) return (long long)g_mv_dl_total * g_mv_dl_pct / 100;
    return 0;
}

int api_mv_download_pct(void)
{
    long long total = api_mv_download_total();

    if ((g_mv_stream_job || g_mv_dl_total > 0) && total > 0) {
        long long done = api_mv_download_bytes();
        int p = (int)(done * 100 / total);

        if (p < 0) p = 0;
        if (p > 99) p = 99;
        return p;
    }
    if (g_mv_stream_job) return 0;
    return g_mv_dl_pct;
}

long long api_mv_download_total(void)
{
    if (g_mv_stream_job && mv_stream_total() > 0) return mv_stream_total();
    return g_mv_dl_total;
}

const MvInfo *api_mv_info(void) { return api_mv_download_state()==1&&g_mv_downloading.id>0?&g_mv_downloading:&g_mv; }

void api_request_export(const Song *song)
{
    queue_job(JOB_EXPORT, 0, NULL, song);
}

void api_clear_lyric(void)
{
    g_lyric_word_count=0;
    g_lyric.count=0;g_lyric.translated_count=0;memset(g_lyric.translation,0,sizeof(g_lyric.translation));
    g_lyric.ms[0] = 0;
    g_lyric.text[0][0] = 0;
}

void api_request_lyric(const Song *song)
{
    queue_job(JOB_LYRIC, 0, NULL, song);
}

void api_request_art(const Song *song)
{
    queue_job(JOB_ART, 0, NULL, song);
}

int api_download_pending(long long id){int found=0;if(!lock_take(g_lock))return 0;for(int i=0;i<g_download_request_n;++i)if(g_download_requests[i].id==id)found=1;lock_give(g_lock);return found;}
static int request_background_download(const Song*song,int silent){
 if(!song||song->id<=0)return -1;if(cache_has(song->id)||dl_has_song(song->id))return 0;
 if(!lock_take(g_lock))return -1;
 for(int i=0;i<g_download_request_n;++i)if(g_download_requests[i].id==song->id){if(!silent)g_download_silent[i]=0;lock_give(g_lock);return 0;}
 if(g_download_request_n>=32){lock_give(g_lock);return -1;}
 int at=g_download_request_n;if(!silent){at=0;while(at<g_download_request_n&&!g_download_silent[at])at++;for(int i=g_download_request_n;i>at;--i){g_download_requests[i]=g_download_requests[i-1];g_download_silent[i]=g_download_silent[i-1];}}
 g_download_requests[at]=*song;g_download_silent[at]=(unsigned char)silent;g_download_request_n++;lock_give(g_lock);return 1;
}
void api_request_download(const Song*song){if(request_background_download(song,0)<0)post(EV_DL_FAIL,0,"后台解析队列满，请稍后再加入");}
int api_request_download_batch(const Song*songs,int count){int added=0;for(int i=0;i<count;++i){int r=request_background_download(&songs[i],0);if(r<0)break;if(r>0)added++;}return added;}
void api_request_prefetch(const Song*song){request_background_download(song,1);}
void api_clear_pending_downloads(void){if(lock_take(g_lock)){g_download_epoch++;g_download_request_n=0;lock_give(g_lock);}}
void api_cancel_pending_prefetch(void){if(lock_take(g_lock)){g_download_epoch++;int count=0;for(int i=0;i<g_download_request_n;++i)if(!g_download_silent[i]){g_download_requests[count]=g_download_requests[i];g_download_silent[count++]=0;}g_download_request_n=count;lock_give(g_lock);}}
int api_pending_download_count(void){int n=0;if(lock_take(g_lock)){n=g_download_request_n;lock_give(g_lock);}return n;}


/* ---------------- 在线更新 ---------------- */

void api_request_update_check(void)
{
    /* 正在下载时不重复入队 */
    if (g_upd_state == 1 || g_upd_state == 4 || g_upd_state==7) return;
    g_upd_state=1;
    queue_job(JOB_UPDATE_CHECK,0,NULL,NULL);
}

void api_request_update_download(void)
{
    if(g_upd_state!=3)return;
    g_upd_abort=0;g_upd_progress=0;g_upd_state=7;
    queue_job(JOB_UPDATE_DL,0,NULL,NULL);
}

void api_update_abort(void) { g_upd_abort = 1; }

int api_update_state(void) { return g_upd_state; }
int api_update_progress(void) { return g_upd_progress; }
const char *api_update_version(void) { return g_upd_ver; }
const char *api_update_changelog(void) { return g_upd_log; }
const char *api_update_message(void) { return g_upd_msg; }
void api_update_set_pending(int p) { g_upd_pending = p ? 1 : 0; }
int api_update_pending(void) { return g_upd_pending; }

const SongList *api_result(void) { return &g_pending; }

void api_free_buffers(void)
{
    image_cache_release();
    lyric_timing_free();
    g_art_cpu_only = 1; /* MV transition: do not reuse hardware JPEG context. */
    if(g_pending_block>=0)sceKernelFreePartitionMemory(g_pending_block);g_pending_block=-1;g_pending_storage=NULL;
    free(g_square_snapshot); g_square_snapshot = NULL; g_discovery_category = -1;
    free(g_buf); g_buf = NULL;
    free(g_jpg); g_jpg = NULL;
    free(g_rgba); g_rgba = NULL;
    free(g_art); g_art = NULL;
    if(g_backdrop_block>=0)sceKernelFreePartitionMemory(g_backdrop_block);g_backdrop_block=-1;
    g_backdrop=NULL;g_backdrop_dim=NULL;
    g_art_w = g_art_h = 0;
}


/* 2026-11-01：ME 会话结束后 app 卡在 api_init 里（mvstep 显示 api-init-begin
 * 之后没有 done）。这里用同样的直写文件方式打点，定位到底卡在哪一步。 */
static void api_step(const char *tag)
{
    char b[96];
    int n = snprintf(b, sizeof(b), "api %-13s free=%dK\n", tag,
                     (int)(sceKernelTotalFreeMemSize() / 1024));
    int fd = sceIoOpen(APP_DIR "mvstep.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);

    if (fd < 0) return;
    sceIoWrite(fd, b, (unsigned)n);
    sceIoClose(fd);
}

void api_init(void)
{
    if (g_buf) return;
    if(!g_pending_storage){
        g_pending_block=sceKernelAllocPartitionMemory(2,"melody_result",PSP_SMEM_High,sizeof(*g_pending_storage),NULL);
        if(g_pending_block>=0)g_pending_storage=sceKernelGetBlockHeadAddr(g_pending_block);
    }
    if(!g_pending_storage){if(g_pending_block>=0)sceKernelFreePartitionMemory(g_pending_block);g_pending_block=-1;mlog("api: result list allocation failed");return;}
    home_cards_load();
    api_step("enter");
    memset(&g_pending, 0, sizeof(g_pending));
    api_step("login");
    login_load();          /* 恢复上次扫码登录的凭据（有的话） */
    api_step("alt");
    alt_init();
    api_step("thumb");
    thumb_disk_init();
    api_step("thumb-done");

    /* 修复：g_lock 初值是 -1，以前写成 if (!g_lock)，-1 是真值 → 这把锁
     * 从来没被创建过，所有 LOCK/UNLOCK 都是对着 SceUID -1 调用（PPSSPP
     * 日志里满屏 UNKNOWN_SEMID=sceKernelWaitSema(-1,...) 就是它），事件队列
     * 和任务队列实际上完全没有互斥。改成 < 0 才会真正建锁。 */
    api_step("lock");
    if (g_lock < 0) g_lock = lock_new("melody_api");
    g_quit = 0;
    /* Worker is stopped here: retire any in-flight batch abandoned at shutdown. */
    g_daily_preview_pending = g_daily_preview_inflight = 0;
    g_thumb_qn = g_av_qn = 0;
    image_cache_alloc();
    for (int i = 0; g_thumbs && i < THUMB_MAX; ++i)
        if (g_thumbs[i].state == 1) { g_thumbs[i].state = 0; g_thumbs[i].url[0] = 0; }
    for (int i = 0; g_avs && i < AV_MAX; ++i)
        if (g_avs[i].state == 1) { g_avs[i].state = 0; g_avs[i].url[0] = 0; }
    if(!net_offline())home_thumbs_restore();

    api_step("malloc");
    g_buf = (char *)malloc(BUF_SIZE);
    g_jpg = (unsigned char *)memalign(64, JPEG_MAX);
    g_rgba = (unsigned char *)memalign(64, ART_MAX * ART_MAX * 4);
    g_art = (unsigned short *)memalign(64, ART_MAX * ART_MAX * 2);
    /* Full-screen backgrounds need not fragment the fixed libc heap used by
     * scripts, HTTP and image decode. Both surfaces retire before MV. */
    g_backdrop_block=sceKernelAllocPartitionMemory(2,"melody_backdrops",PSP_SMEM_High,SCR_W*SCR_H*4,NULL);
    if(g_backdrop_block>=0)g_backdrop=sceKernelGetBlockHeadAddr(g_backdrop_block);
    if(g_backdrop)g_backdrop_dim=g_backdrop+SCR_W*SCR_H;

    /* 修复：必需缓冲缺任何一个就记日志并放弃启动 api 线程。 */
    api_step("malloc-done");
    if (!g_buf || !g_jpg || !g_rgba || !g_art || !g_backdrop || !g_backdrop_dim) {
        mlog("api: out of memory (buf=%d jpg=%d rgba=%d art=%d bg=%d dim=%d), api disabled",
             g_buf != NULL, g_jpg != NULL, g_rgba != NULL, g_art != NULL,
             g_backdrop != NULL, g_backdrop_dim != NULL);
        return;
    }

    api_step(g_jpeg_on ? "jpeg-skip" : "jpeg");
    if (!g_jpeg_on && sceJpegInitMJpeg() >= 0) g_jpeg_on = 1;

    api_step("thread");
    g_thid = sceKernelCreateThread("melody_api", worker, WORKER_PRIO, WORKER_STACK, 0, NULL);
    if (g_thid >= 0) sceKernelStartThread(g_thid, 0, NULL);

    api_step("ready");
    mlog("api ready");
}

int api_shutdown(void)
{    SceUInt timeout = 3000 * 1000;
    int r;
    int forced = 0;

    g_quit=1;g_abort=1;
    if (g_thid >= 0) {
        r = sceKernelWaitThreadEnd(g_thid, &timeout);
        if (r < 0) {
            mlog("api thread did not exit in 3s (0x%08X), terminate", r);
            sceKernelTerminateDeleteThread(g_thid);
            forced = 1;
        } else {
            sceKernelDeleteThread(g_thid);
        }
        g_thid = -1;
    }
    if (forced) {
        /* 被强杀的线程可能正握着 g_lock（事件/任务队列）或 io_lock（stdio）：
         * 它一死这两把锁就永远回不来，主线程下一句 api_clear_events() 或
         * mlog() 就永久冻结。这里直接把两把锁换成新的，把毒彻底解掉。 */
        io_lock_reset();          /* 先换 stdio 锁，下面那几行日志才写得出去 */
        api_reset_locks();
        /* 2026-11-01：net 的 g_get_lock/g_conn_lock 也是无限等，被强杀的线程
         * 若停在 http_get 里就会把锁永久带走（真机：播完 MV 网络死）。 */
        net_reset_locks();
        mlog("api: forced kill, locks recreated (io timeouts=%d)", io_lock_timeouts());
    }
    /* The worker is stopped now: release skin admission and recover published
     * directories before another worker can observe a half-built catalog. */
    if(g_skin_request_busy){g_skin_request_busy=0;skin_catalog_load_local();}
    /* 线程可能是被强杀的：把空闲连接全丢掉，别让下一个请求复用半死的 socket */
    net_drop_idle();
    /* 2026-11-01：这里不再 Finish JPEG。
     * 实测：ME 会话（Custom Core 桥）跑完、PRX 卸载后，再调一次
     * sceJpegInitMJpeg() 会整机卡死（mvstep 停在 "api jpeg"）。
     * JPEG 模块是系统常驻的用户库，app 存续期间一直 Init 着无害，
     * 反而避开了"ME 会话后再初始化 JPEG"这条会挂的路径。 */
    (void)g_jpeg_on;
    return forced;
}

long long api_comment_song(void) { return g_comment_song; }
unsigned int api_comment_generation(void) { return g_comment_request_gen; }
const char *api_comment_name(void) { return g_comment_requested_song == g_comment_song ? g_comment_name : g_comment_requested_name; }
long long api_comment_target(void) { return g_comment_requested_song; }

void api_request_announcement_check(void){queue_job(JOB_ANNOUNCEMENT_CHECK,community_announcement_state()!=COMMUNITY_ANNOUNCEMENT_IDLE,NULL,NULL);}
void api_request_announcement_dismiss(int ignore){queue_job(JOB_ANNOUNCEMENT_DISMISS,ignore!=0,NULL,NULL);}
