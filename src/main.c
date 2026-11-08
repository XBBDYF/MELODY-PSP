/*
 * MELODY PSP — 主程序
 * ------------------------------------------------------------------
 * 线程：主线程画界面（60fps），api 线程取数据，dl 线程下载，audio 线程出声。
 * 所有网络请求都是「投递任务 → 轮询事件」，界面永远不等网络。
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <psputility.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "comment_view.h"
#include "update_pbp.h"
#include "alt.h"
#include "app.h"
#include "audio.h"
#include "cache.h"
#include "local.h"
#include "mp3dur.h"
#include "sysinfo.h"
#include "dl.h"
#include "gfx.h"
#include "log.h"
#include "net.h"
#include "store.h"
#include "ui.h"
#include "skin_runtime.h"
#include "skin_options_ui.h"
#include "ime.h"
#include "search_history.h"
#include "search_chips.h"
#include "update_gallery.h"
#include "skin_transition.h"
#include "skin_sound.h"
#include "skin_disc.h"
#include "skin_catalog.h"
#include "mv.h"
#include "settings_order.h"
#include "next_plan.h"
#include "playback_context.h"
#include "update_log_view.h"
#include "community.h"
#include "mvexport.h"

/* src/mv.c: 1 = 可用内存不足被闸门拦下（第二次进 MV 的常见情况） */
extern int mv_block_reason(void);

AppState g_app;
SongList g_list;
int g_volume = 80;

static char g_list_title[48];
static int g_list_kind;
static const char *cat_names[8] = { "华语", "流行", "摇滚", "电子", "民谣", "说唱", "轻音乐", "古典" };

static SceInt64 g_start_us;
static unsigned int g_prev_btn;
static unsigned int g_pending_press;   /* 绘制期间按下、还没处理过的键 */
static int g_update_applied;           /* 启动时换上了 EBOOT.NEW，提示再进一次 */
static int g_exit_confirm;
static int g_shot_index;
static int g_play_fail;
static int g_fail_streak;   /* 连续播放失败次数（只在成功后清零） */
static float g_fail_t;      /* 上次自动换歌的时间（限速，防止死循环） */
static int g_end_done;
static Song g_export_song;      /* 待覆盖导出的歌 */
static int g_retry_gave_up;
static int g_retry_waiting;
static int policy_open,policy_scroll;
static int notice_open,notice_scroll,notice_ignore=COMMUNITY_ANNOUNCEMENT_DEFAULT_IGNORE,notice_seen,notice_requested;
static int preserve_play_page;
static int g_auto_skip;      /* 连续自动跳歌次数（放不了就跳，最多 5 首） */
/* 缓存已经完整、播放却没起来时的自动救场（每首歌最多试几次） */
static long long g_auto_fix_id = -1;
static int g_auto_fix_n;
static float g_auto_fix_t;  /* 最近一次恢复请求：每首歌最多四次，且保留限速 */
static int g_auto_wait_active, g_auto_wait_fired, g_auto_wait_pos;
static float g_auto_wait_t; /* 同歌同进度连续 BUFFER/ERROR 的实际等待起点 */
static float g_seek_guard_t; /* 刚快进/快退过：这段时间内别自动重开播 */
static float g_retry_t;
static int g_retry_n;
/* Playback position once the current start has proven itself (2s); 0 = not yet. */
static int g_play_stable_ms;
static int g_dev = -1;
static int g_autoplay;
static int g_frame;
static volatile int g_exit_req;
static int g_cache_gen_seen = -1;
static int g_dl_gen_seen = -1;
static long long g_dl_focus_song;
static long long g_like_subject;
static unsigned int g_like_account_gen;
static unsigned int g_like_request_gen;

/* 按键自动重复 */
static unsigned int g_rep_btn;
static float g_rep_t;

/* 自动化测试 */
static float g_test_t;
static int g_test_step = -1;
static float g_shot_t = -1;
static int g_shot_done;

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static float now_sec(void)
{
    return (float)((sceKernelGetSystemTimeWide() - g_start_us) / 1000000.0);
}

/* 上次检查更新下载好的 EBOOT.NEW：校验 PBP 头和大小后替换 EBOOT.PBP，
 * 旧版留成 EBOOT.OLD，新版本起不来可以用 USB 改回去。 */
static void update_apply_startup(void)
{
    SceIoStat st;
    unsigned char magic[40];
    int fd;

    /* A previous apply can fail both commit and rollback. Recover the only
     * old program first; this invocation never attempts another update. */
    if (sceIoGetstat(APP_DIR "EBOOT.PBP", &st) < 0 &&
        sceIoGetstat(APP_DIR "EBOOT.OLD", &st) >= 0) {
        if (sceIoRename(APP_DIR "EBOOT.OLD", APP_DIR "EBOOT.PBP") < 0)
            mlog("update: recovery failed; EBOOT.OLD retained");
        else
            mlog("update: recovered EBOOT.OLD; update deferred");
        return;
    }
    if (sceIoGetstat(APP_DIR "EBOOT.NEW", &st) < 0) return;
    api_update_set_pending(1);
    if (st.st_size < 1000000) {
        sceIoRemove(APP_DIR "EBOOT.NEW");
        api_update_set_pending(0);
        return;
    }

    /* 下载时写下的期望大小（update.meta）。没有它、或对不上，一律拒绝：
     * 2026-11-01 真机上就是一份 2,492,074 字节的残缺 EBOOT 被替换进
     * EBOOT.PBP，重启直接黑屏——只校验 "\0PBP" 头是拦不住的。 */
    {
        char mb[32];
        long long want = 0;
        int mfd = sceIoOpen(APP_DIR "update.meta", PSP_O_RDONLY, 0);

        if (mfd >= 0) {
            int n = sceIoRead(mfd, mb, sizeof(mb) - 1);

            sceIoClose(mfd);
            if (n > 0) {
                mb[n] = 0;
                want = atoll(mb);
            }
        }
        if (want <= 0 || (long long)st.st_size != want) {
            sceIoRemove(APP_DIR "EBOOT.NEW");
            sceIoRemove(APP_DIR "update.meta");
            api_update_set_pending(0);
            mlog("update: EBOOT.NEW %lldB != meta %lldB, removed",
                 (long long)st.st_size, want);
            return;
        }
    }

    fd = sceIoOpen(APP_DIR "EBOOT.NEW", PSP_O_RDONLY, 0);
    if (fd < 0) return;
    if (sceIoRead(fd, magic, sizeof(magic)) != sizeof(magic) ||
        !update_pbp_valid(magic, sizeof(magic), (unsigned long long)st.st_size)) {
        sceIoClose(fd);
        sceIoRemove(APP_DIR "EBOOT.NEW");
        api_update_set_pending(0);
        mlog("update: EBOOT.NEW bad magic, removed");
        return;
    }
    if (sceIoClose(fd) < 0) return;

    /* Retire a backup only while a readable, structurally valid live PBP
     * exists. Small paired loaders are valid: no update-size minimum here. */
    if (sceIoGetstat(APP_DIR "EBOOT.PBP", &st) < 0) return;
    fd = sceIoOpen(APP_DIR "EBOOT.PBP", PSP_O_RDONLY, 0);
    if (fd < 0) return;
    {
        int live_ok = sceIoRead(fd, magic, sizeof(magic)) == sizeof(magic) &&
            update_pbp_valid(magic, sizeof(magic), (unsigned long long)st.st_size);
        if (sceIoClose(fd) < 0) live_ok = 0;
        if (!live_ok) { mlog("update: live PBP invalid; backup retained"); return; }
    }
    if (sceIoGetstat(APP_DIR "EBOOT.OLD", &st) >= 0 &&
        sceIoRemove(APP_DIR "EBOOT.OLD") < 0) return;
    if (sceIoRename(APP_DIR "EBOOT.PBP", APP_DIR "EBOOT.OLD") < 0) {
        mlog("update: rename EBOOT.PBP -> EBOOT.OLD failed");
        return;
    }
    if (sceIoRename(APP_DIR "EBOOT.NEW", APP_DIR "EBOOT.PBP") < 0) {
        mlog("update: rename EBOOT.NEW failed, rollback");
        if (sceIoRename(APP_DIR "EBOOT.OLD", APP_DIR "EBOOT.PBP") < 0)
            mlog("update: rollback failed; EBOOT.OLD retained for recovery");
        return;
    }
    api_update_set_pending(0);
    g_update_applied = 1;
    sceIoRemove(APP_DIR "update.meta");
    mlog("update: applied, old saved as EBOOT.OLD");
}

static int dev_mode(void){
    if (g_dev >= 0) return g_dev;
    g_dev = 0;
    {
        FILE *fp;
        io_lock();
        fp = fopen(APP_DIR "dev.flag", "r");
        if (fp) {
            char buf[16] = "";

            if (fgets(buf, sizeof(buf), fp)) g_dev = atoi(buf);
            fclose(fp);
            io_unlock();
            if (g_dev < 1) g_dev = 1;
            mlog("dev flag = %d", g_dev);
        } else {
            io_unlock();
            mlog("dev flag missing");
        }
    }
    return g_dev;
}

static void shot(const char *tag)
{
    char path[96];

    if (!dev_mode()) return;
    if (g_shot_index >= 80) return;

    sceIoMkdir(APP_DIR "shots", 0777);
    snprintf(path, sizeof(path), APP_DIR "shots/%02d_%s.bmp", g_shot_index, tag);
    gfx_dump_bmp(path);
    mlog("shot %s", path);
    g_shot_index++;
}

typedef struct {
    int valid, pending, kind, selection, scroll, kb_edit, state;
    char query[sizeof(g_app.query)];
} SearchReturnContext;
static SearchReturnContext g_search_return;

static void search_return_save(void)
{
    g_search_return.valid = g_search_return.pending = 1;
    g_search_return.kind = g_app.search_kind;
    g_search_return.selection = g_app.list_sel;
    g_search_return.scroll = g_app.list_scroll;
    g_search_return.kb_edit = g_app.kb_edit;
    g_search_return.state = g_app.search_state;
    snprintf(g_search_return.query, sizeof(g_search_return.query), "%s", g_app.query);
}

static int search_return_accept(void)
{
    if (!g_search_return.pending) return 1;
    g_search_return.pending = 0;
    /* An entity open may complete after the user left or changed the query. */
    if (g_app.screen == SC_SEARCH &&
        g_app.search_kind == g_search_return.kind &&
        !strcmp(g_app.query, g_search_return.query)) return 1;
    g_search_return.valid = 0;
    return 0;
}

static struct { int valid, selection, page, category, focus; } g_discovery_return;
static int g_xmb_skin_parent = SC_HOME;
static int xmb_active(void){return skin_runtime_active()&&!strcmp(skin_runtime_style()->id,"xmb");}
/* Explicit content parents; visiting another page never changes this hierarchy. */
static int xmb_parent_screen(int screen)
{
    switch (screen) {
    case SC_LYRICS: case SC_COMMENTS: return SC_PLAYER;
    case SC_LIST:
        if (g_app.list_from == SC_SQUARE || g_app.list_from == SC_GRID)
            return g_app.list_from;
        return SC_HOME;
    case SC_PLAYER:
        if (g_app.play_from == SC_LIST || g_app.play_from == SC_SEARCH ||
            g_app.play_from == SC_CACHE) return g_app.play_from;
        return SC_HOME;
    case SC_LOGIN: return g_app.login_from == SC_SETTINGS ? SC_SETTINGS : SC_HOME;
    case SC_SKINS: return g_xmb_skin_parent == SC_SETTINGS ? SC_SETTINGS : SC_HOME;
    default: return SC_HOME;
    }
}
static void set_screen(int sc)
{
    if (g_app.screen == sc) return;
    if (g_app.screen == SC_SQUARE && sc == SC_LIST) {
        g_discovery_return.valid = 1;
        g_discovery_return.selection = g_app.grid_sel;
        g_discovery_return.page = g_app.grid_page;
        g_discovery_return.category = g_app.cat_sel;
        g_discovery_return.focus = g_app.cat_focus;
    }
    if (g_app.screen == SC_LIST && sc == SC_SQUARE && g_discovery_return.valid) {
        g_app.grid_sel = g_discovery_return.selection;
        g_app.grid_page = g_discovery_return.page;
        g_app.cat_sel = g_discovery_return.category;
        g_app.cat_focus = g_discovery_return.focus;
        g_app.grid_kind = 4;
        g_discovery_return.valid = 0;
    }
    if (sc == SC_SEARCH && g_app.screen == SC_LIST && g_search_return.valid &&
        g_app.search_kind == g_search_return.kind &&
        !strcmp(g_app.query, g_search_return.query)) {
        g_app.search_kind = g_search_return.kind;
        g_app.list_sel = g_search_return.selection;
        g_app.list_scroll = g_search_return.scroll;
        g_app.kb_edit = g_search_return.kb_edit;
        g_app.search_state = g_search_return.state;
        g_search_return.valid = 0;
    }
    skin_transition_request(g_app.screen,sc,g_app.time);
    mlog("set_screen %d -> %d", g_app.screen, sc);
    g_app.screen = sc;
    g_app.download_select_mode=0;g_app.download_selected_count=0;
    g_app.enter = 0.0f;
    g_app.spinner = 0;
}

static void xmb_go_back(void){
 if(!xmb_active())return;
 g_app.cards_open=0;g_app.play_all_next=0;
 set_screen(xmb_parent_screen(g_app.screen));
}

/* ------------------------------------------------------------------ */
/* 播放控制                                                            */
/* ------------------------------------------------------------------ */

static NextPlan g_next_plan;
/* The app PRX already reserves a fixed 2MiB newlib heap. Keep this full-size
 * snapshot there instead of adding 253KiB to the module's resident BSS. */
static PlaybackContext *g_playback_storage;
#define g_playback (*g_playback_storage)

static int playback_context_init(void)
{
    if (g_playback_storage) return 0;
    g_playback_storage = calloc(1, sizeof(*g_playback_storage));
    if (!g_playback_storage) {
        mlog("playback: context allocation failed (%u bytes), exiting before API startup",
             (unsigned)sizeof(*g_playback_storage));
        return -1;
    }
    mlog("playback: context in root heap (%u bytes)", (unsigned)sizeof(*g_playback_storage));
    return 0;
}

static void play_context_index(int index);
static void prefetch_tick(void)
{
    static float checked;
    if (g_app.time - checked < .8f) return;
    checked = g_app.time;
    if (g_playback.source != PLAYBACK_ONLINE || !g_app.has_now) return;
    SongList *list = &g_playback.data.online;
    int index = g_playback.index;
    if (index < 0 || index >= list->count || list->songs[index].id != g_app.now.id) return;
    if (g_app.screen == SC_MV || audio_state() != AUDIO_PLAY || g_app.busy) return;
    next_plan_build(&g_next_plan, list, index, g_app.play_mode);
    int count = store_prefetch_count();
    for (int j = 0; j < g_next_plan.count && j < count; ++j) {
        long long id = g_next_plan.ids[j];
        if (g_next_plan.requested_mask & (1u << j)) continue;
        if (cache_has(id) || dl_has_song(id) || api_download_pending(id)) continue;
        for (int i = 0; i < list->count; ++i) if (list->songs[i].id == id) {
            g_next_plan.requested_mask |= 1u << j;
            api_request_prefetch(&list->songs[i]);
            return;
        }
    }
}

static void play_online_context_index(int index)
{
    if (index < 0 || index >= g_playback.count) return;
    g_playback.index = index;

    /* 不预跳过：列表里的「VIP」只是高音质标，128k 标准音质往往能播。
     * 一律先试着取地址，真 403 才由播放失败逻辑跳下一首。 */

    g_app.playing_index = index;
    g_app.now = g_playback.data.online.songs[index];
    g_app.is_liked = 0;
    g_app.has_now = 1;
    g_app.paused = 0;
    g_app.lyrics_open = 0;
    g_app.lyric_scroll = 0;
    g_app.marquee = 0;
    g_play_fail = 0;
    g_retry_gave_up = 0;
    g_retry_n = 0;
    g_retry_waiting=1;
    g_app.busy = 1;

    mlog("play id=%lld name=[%s] artist=[%s] album=[%s]", g_app.now.id, g_app.now.name, g_app.now.artist, g_app.now.album);
    store_push_recent(&g_app.now);
    if (!preserve_play_page && g_app.screen != SC_PLAYER) {
        g_app.play_from = g_app.screen;
        g_app.play_from_tab = g_app.cache_tab;
    }
    api_request_play(index, &g_app.now);
    if(!preserve_play_page)set_screen(SC_PLAYER);
}

static void play_index(int index)
{
    if (index < 0 || index >= g_list.count) return;
    playback_context_online(&g_playback, &g_list, index);
    play_online_context_index(index);
}

static void play_song_obj(const Song *s, int index)
{
    g_app.playing_index = index;
    g_app.now = *s;
    g_app.is_liked = 0;
    g_app.has_now = 1;
    g_app.paused = 0;
    g_app.lyrics_open = 0;
    g_app.lyric_scroll = 0;
    g_app.marquee = 0;
    g_play_fail = 0;
    g_retry_gave_up = 0;
    g_retry_n = 0;
    g_retry_t = 0;
    g_retry_waiting = 1;
    g_app.busy = 1;
    if (!preserve_play_page && g_app.screen != SC_PLAYER) {
        g_app.play_from = g_app.screen;
        g_app.play_from_tab = g_app.cache_tab;
    }

    store_push_recent(&g_app.now);
    api_request_play(index, &g_app.now);
    if (!preserve_play_page) set_screen(SC_PLAYER);
}

static void play_prev(void)
{
    if (!g_app.has_now) return;
    g_auto_skip = 0;
    int index = playback_context_next(&g_playback, &g_next_plan, -1, g_app.play_mode);
    if (index >= 0) play_context_index(index);
}

static void play_next(int dir)
{
    if (!g_app.has_now) return;
    int index = playback_context_next(&g_playback, &g_next_plan, dir, g_app.play_mode);
    if (index >= 0) play_context_index(index);
    else mlog("auto next: no more songs in context dir=%d mode=%d src=%d", dir, g_app.play_mode,
              g_playback.source);
}

/* 缓存目录里有 mp3、但索引里没有（老版本下载没收尾）：认领回来，
 * 名字优先用「最近播放」里的同 id 记录，没有就显示占位名 */
static void adopt_cache_orphans(void)
{
    SceUID d;
    SceIoDirent ent;
    int adopted = 0;

    d = sceIoDopen("./cache");
    if (d < 0) return;

    for (;;) {
        char *dot;
        long long id;
        const Song *meta = NULL;
        char path[128];
        int kbps = 0, dur = 0;

        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (!(ent.d_stat.st_mode & FIO_S_IFREG)) continue;

        dot = strrchr(ent.d_name, '.');
        if (!dot || strcasecmp(dot, ".mp3")) continue;
        if (ent.d_stat.st_size < 64 * 1024) continue;

        id = (long long)atoll(ent.d_name);
        if (id <= 0 || cache_find(id)) continue;

        /* 有 <id>.part 说明上次没下完（取消/切歌/退出），不能当完整缓存认领 */
        {
            SceIoStat pst;
            char ppath[128];

            snprintf(ppath, sizeof(ppath), APP_DIR "cache/%lld.part", id);
            if (sceIoGetstat(ppath, &pst) >= 0) {
                mlog("cache skip partial %lld (has .part)", id);
                continue;
            }
        }

        for (int i = 0; i < store_recent_count(); ++i) {
            const Song *r = store_recent_get(i);

            if (r && r->id == id) { meta = r; break; }
        }

        snprintf(path, sizeof(path), "./cache/%s", ent.d_name);

        if (!audio_file_ok(path)) {
            /* 只跳过不删除：文件可能正在被播放 */
            mlog("cache skip suspicious %lld", id);
            continue;
        }

        dur = audio_probe_duration(path, &kbps);

        cache_adopt(id, meta ? meta->name : "缓存歌曲",
                    meta ? meta->artist : "",
                    meta ? meta->dur_ms : dur, (int)ent.d_stat.st_size);
        mlog("cache adopt %lld %s (%dKB, %dms, %dkbps)", id,
             meta ? meta->name : "-", (int)(ent.d_stat.st_size / 1024), dur, kbps);
        adopted++;
    }
    sceIoDclose(d);

    if (adopted) mlog("cache adopt: %d orphans", adopted);
}

/* 启动时清掉上次没下完的东西：有 <id>.part 说明文件可能是半截的，
 * 连 mp3 一起删（不能只删标记，否则 cache_prune 会保留 >64KB 的大文件，
 * 下一次启动又会被 adopt 当成完整缓存认领）。 */
static void drop_partial_downloads(void)
{
    SceUID d;
    SceIoDirent ent;
    long long ids[32];
    int n = 0, removed = 0;

    d = sceIoDopen(APP_DIR "cache");
    if (d < 0) return;

    for (;;) {
        size_t len;

        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (!(ent.d_stat.st_mode & FIO_S_IFREG)) continue;
        len = strlen(ent.d_name);
        if (len <= 5 || strcasecmp(ent.d_name + len - 5, ".part")) continue;
        if (n < (int)(sizeof(ids) / sizeof(ids[0]))) ids[n++] = atoll(ent.d_name);
    }
    sceIoDclose(d);

    for (int i = 0; i < n; ++i) {
        char p[128];

        if (ids[i] > 0) {
            cache_path(p, sizeof(p), 0, ids[i]); sceIoRemove(p);   /* mp3 */
            cache_path(p, sizeof(p), 2, ids[i]); sceIoRemove(p);   /* 封面 */
            cache_path(p, sizeof(p), 3, ids[i]); sceIoRemove(p);   /* 歌词 */
        }
        snprintf(p, sizeof(p), APP_DIR "cache/%lld.part", ids[i]);
        sceIoRemove(p);
        removed++;
    }
    if (removed) mlog("cache: dropped %d partial downloads", removed);
}

static void play_local_path(const char *path)
{
    const char *name = path ? strrchr(path, '/') : NULL;
    name = name ? name + 1 : path;
    Song s;
    int len;

    if (!path) return;

    memset(&s, 0, sizeof(s));
    s.id = -1;
    s.dur_ms = 0;

    /* 优先用 ID3 标题（文件名是 GBK，直接显示会变成 ???u） */
    if (!api_local_meta(path, &s) || !s.name[0]) {
        int k = 0;

        snprintf(s.name, sizeof(s.name), "%s", name ? name : "本地音乐");
        len = (int)strlen(s.name);
        if (len > 4 && !strcasecmp(s.name + len - 4, ".mp3")) s.name[len - 4] = 0;
        /* 非 ASCII（GBK 汉字）替成 ?，至少不会乱码 */
        for (int i = 0; s.name[i]; ++i) {
            if ((unsigned char)s.name[i] >= 0x80) {
                s.name[k++] = '?';
                /* 跳过后续的 GBK 尾字节 */
                while ((unsigned char)s.name[i + 1] >= 0x80 && (unsigned char)s.name[i + 1] < 0xC0) i++;
            } else {
                s.name[k++] = s.name[i];
            }
        }
        s.name[k] = 0;
    }

    if (audio_play_file(path, &s) < 0) {
        ui_toast(audio_error());
        return;
    }

    {
        int art_ok = api_local_art(path);   /* 本地文件的内嵌封面 */
        long long fid = mp3_id_from_path(path);

        if (art_ok != 0 && fid > 0) {
            /* 文件名就是网易 id（缓存/导出文件）：一次详情请求，最准 */
            api_request_local_id(fid);
        } else if (art_ok != 0 && s.name[0] && !strchr(s.name, '?')) {
            /* 按歌名+歌手+精确时长匹配；时长用逐帧扫描，别用第一帧码率估算 */
            int dur = mp3_dur_ms(path, NULL);

            if (dur <= 0) dur = audio_dur_ms();
            api_request_local_meta(s.name, s.artist, dur);
        }
    }

    g_app.now = s;
    g_app.is_liked = 0;
    g_app.has_now = 1;
    g_app.paused = 0;
    g_app.playing_index = -1;
    g_app.lyrics_open = 0;
    g_app.lyric_scroll = 0;
    g_app.marquee = 0;
    g_play_fail = 0;
    g_app.busy = 0;
    if (!preserve_play_page && g_app.screen != SC_PLAYER) {
        g_app.play_from = SC_CACHE;
        g_app.play_from_tab = 1;
    }
    api_clear_lyric();          /* 上一首的歌词不能留着 */
    g_retry_gave_up = 0;
    g_retry_n = 0;
    g_retry_t = 0;
    g_retry_waiting = 0;
    if (!preserve_play_page) set_screen(SC_PLAYER);
    mlog("play local %s", path);
}

static void play_local_file(int i)
{
    int count = local_count();
    if (i < 0 || i >= count || !local_path_of(i)) return;
    playback_context_local(&g_playback, count, i, local_path_of);
    play_local_path(g_playback.data.paths[i]);
}

static void play_cached_context_index(int index)
{
 if(g_playback.source!=PLAYBACK_CACHE||index<0||index>=g_playback.count)return;
 long long id=0;FILE*queue=fopen(APP_DIR "cache-playback.ids","rb");if(!queue)return;int okay=fseek(queue,(long)index*8,SEEK_SET)==0&&fread(&id,8,1,queue)==1;fclose(queue);if(!okay)return;
 const CacheEntry*entry=cache_find(id);if(!entry||!cache_has(entry->id))return;
 Song song;memset(&song,0,sizeof(song));song.id=entry->id;song.dur_ms=entry->dur_ms;snprintf(song.name,sizeof(song.name),"%s",entry->name);snprintf(song.artist,sizeof(song.artist),"%s",entry->artist);snprintf(song.album,sizeof(song.album),"%s",entry->album);snprintf(song.art,sizeof(song.art),"%s",entry->art);song.playable=1;g_playback.index=index;play_song_obj(&song,-1);
}
static void play_cache_entry(int index)
{
 int count=cache_count();if(index<0||index>=count)return;
 int copied=cache_snapshot_ids(APP_DIR "cache-playback.ids");if(copied<1||index>=copied){ui_toast("缓存播放队列保存失败");return;}
 g_playback.source=PLAYBACK_CACHE;g_playback.count=copied;g_playback.index=index;
 play_cached_context_index(index);
}

static void play_context_index(int index)
{
    if (index < 0 || index >= g_playback.count) return;
    switch (g_playback.source) {
    case PLAYBACK_ONLINE: play_online_context_index(index); break;
    case PLAYBACK_CACHE: play_cached_context_index(index); break;
    case PLAYBACK_LOCAL:
        g_playback.index = index;
        play_local_path(g_playback.data.paths[index]);
        break;
    default: break;
    }
}

static void download_current(void)
{
    if (!g_app.has_now && g_list.count == 0) return;

    if (g_app.screen == SC_LIST && g_list.count > 0) {
        const Song *s = &g_list.songs[g_app.list_sel];
        if (cache_has(s->id)) { ui_toast("这首已经在缓存里了"); return; }
        api_request_download(s);
        g_app.busy = 1;
    } else if (g_app.has_now) {
        if (cache_has(g_app.now.id)) { ui_toast("这首已经在缓存里了"); return; }
        api_request_download(&g_app.now);
        g_app.busy = 1;
    }
}

/* ------------------------------------------------------------------ */
/* 事件                                                                */
/* ------------------------------------------------------------------ */

static void refresh_profiles(void)
{
    int n = net_list_profiles((NetProfile *)g_app.profiles, MAX_PROFILES);

    g_app.profile_count = n;
    if (g_app.profile_sel >= n) g_app.profile_sel = 0;
    g_app.wlan_on = net_wlan_switch() == 1;
    mlog("profiles %d, wlan %d", n, g_app.wlan_on);
}

static void open_wifi_screen(void)
{
    refresh_profiles();
    g_app.connecting = 0;
    g_app.connect_msg[0] = 0;
    set_screen(SC_WIFI);
}

/* 翻唱/改版标记：这些版本不该压在原唱前面。
 * 只收明确的版本词，避免 "Live Forever"、"Deliver" 这类正常标题被误伤。 */
static const char *const k_cover_markers[] = {
    "翻唱", "翻自", "翻奏", "cover", "Cover", "COVER", "伴奏", "纯音乐",
    "instrumental", "Instrumental", "karaoke", "Karaoke", "KTV", "ktv", "卡拉OK",
    "消音", "DJ版", "dj版", "remix", "Remix", "REMIX", "8bit", "8Bit", "钢琴版",
    "吉他版", "女声版", "男声版", "童声版", "抖音", "慢速", "加速", "降调", "升调",
    "(Live", "[Live", "（Live", "Live版", "live版", "现场版", "演唱会", "片段", "铃声", "试听版"
};

static int is_cover_version(const char *name)
{
    for (unsigned i = 0; i < sizeof(k_cover_markers) / sizeof(k_cover_markers[0]); ++i)
        if (strstr(name, k_cover_markers[i])) return 1;
    return 0;
}

/* 搜索排序分：原唱 > 正式版 > 翻唱/改版。
 * 网易公开索引经常把翻唱排在前面，这里显式拉开差距，并让查询词命中的歌手/歌名加分。 */
static int song_score(const Song *s)
{
    int sc = s->pop;
    const char *q = g_list_title;

    if (is_cover_version(s->name)) sc -= 150;
    if (q[0]) {
        if (strstr(s->artist, q)) sc += 90;        /* 查询词就是歌手：原唱优先 */
        else if (strstr(s->name, q)) sc += 40;
    }
    if (s->fee < 0) sc += 30;                      /* 第三方源补进来的行，确定能放 */
    else if (s->playable == 1) sc += 10;
    return sc;
}

/* 搜索结果：原唱优先，然后按分数稳定排序；前几首标「热门」（稳定插入排序） */
static void sort_search_list(void)
{
    static int score[MAX_SONGS];
    int i, j;

    for (i = 0; i < g_list.count; ++i) score[i] = song_score(&g_list.songs[i]);
    for (i = 1; i < g_list.count; ++i) {
        Song tmp = g_list.songs[i];
        int tsc = score[i];

        for (j = i - 1; j >= 0 && score[j] < tsc; --j) {
            g_list.songs[j + 1] = g_list.songs[j];
            score[j + 1] = score[j];
        }
        g_list.songs[j + 1] = tmp;
        score[j + 1] = tsc;
    }
    for (i = 0; i < g_list.count; ++i)
        g_list.songs[i].hot = (i < 3 && g_list.songs[i].pop > 0) ? 1 : 0;
}

static void copy_list_from_api(void)
{
    const SongList *src = api_result();

    /* Copy row by row (not a 260KB memcpy) but keep every result: songs without
     * an official URL stay listed and play/download through the alternative
     * sources. The copy itself is mandatory — dropping it left the list count
     * non-zero while g_list.songs[] still held stale entries. */
    g_list.count = src->count > MAX_SONGS ? MAX_SONGS : src->count;
    for (int i = 0; i < g_list.count; ++i) g_list.songs[i] = src->songs[i];
    g_list.chart_id = src->chart_id;
    g_list.kind = src->kind;
    snprintf(g_list.title, sizeof(g_list.title), "%s", src->title);
    snprintf(g_list_title, sizeof(g_list_title), "%s", g_list.title);
    g_list_kind = g_list.kind;
    g_app.list_sel = 0;
    g_app.list_scroll = 0;
    g_app.list_more = 0;
    g_app.list_total = 0;

    if (g_list_kind == LK_SEARCH) sort_search_list();

    /* 服务端总数：歌单/我喜欢/搜索都能拿到，界面用它显示 "x/y 首" 并触发续页 */
    g_app.list_total = api_list_total();
    if (g_app.list_total < g_list.count) g_app.list_total = g_list.count;
}

/* 只有「用户还停在能接受列表的页面」时才允许后台请求把界面切走。
 * 以前 EV_LIST_OK/EV_CARDS_OK 无条件 set_screen()，首页推荐的请求一旦晚到
 * （用户已经在播放页/歌词/设置/MV），就会把人硬拽进歌单网格。 */
static int can_auto_navigate(void)
{
    switch (g_app.screen) {
    case SC_BOOT:
    case SC_HOME:
    case SC_LIST:
    case SC_SEARCH:
    case SC_GRID:
    case SC_SQUARE:
    case SC_MINE:
        return 1;
    default:
        return 0;
    }
}

static void pump_events(void)
{
    ApiEvent ev;

    /* MV 播放期间不处理任何会切界面的事件，只把队列排空 */
    if (g_app.screen == SC_MV) {
        while (api_poll(&ev)) { }
        return;
    }

    while (api_poll(&ev)) {
        switch (ev.kind) {
        case EV_NET_OK:
            g_app.wifi_state = 0;
            g_app.online = 1;
            /* 开机联网后静默查一次更新；结果在回到主页时弹窗 */
            if (!g_app.upd_checked) {
                g_app.upd_checked = 1;
                if(dev_mode()!=67)api_request_update_check(); /* Mode67 selects Home/Settings before checking. */
            }
            if (!g_app.wifi_name[0])
                snprintf(g_app.wifi_name, sizeof(g_app.wifi_name), "在线");
            g_app.net_msg[0] = 0;
            g_app.connecting = 2;
            snprintf(g_app.connect_msg, sizeof(g_app.connect_msg), "已连接");
            if (api_logged_in()) api_request_liked_total();
            if (g_app.screen == SC_BOOT || g_app.screen == SC_WIFI) {
                set_screen(SC_HOME);
                g_app.tab = 0;
                if (g_app.screen == SC_HOME) {
                    g_app.busy = 1;
                    api_request_chart(0);
                    /* 首页「推荐歌单」：拿到卡片后不切屏，留在发现页显示 */
                    g_app.cards_stay = 1;
                    api_request_recommend_lists();
                }
            }
            break;

        case EV_NET_FAIL:
            g_app.wifi_state = 1;
            g_app.online = 0;
            if (g_app.connecting == 1) {
                g_app.connecting = 3;
                snprintf(g_app.connect_msg, sizeof(g_app.connect_msg), "连接失败：%s",
                         ev.msg[0] ? ev.msg : "检查热点与密码");
            } else {
                g_app.connecting = 0;
            }
            snprintf(g_app.net_msg, sizeof(g_app.net_msg), "网络未连接");
            if (g_app.screen == SC_BOOT) open_wifi_screen();
            break;

        case EV_SKIN_INSTALLED:{char root[256],err[192]={0};if(skin_catalog_root(ev.msg,root,sizeof(root))==0&&skin_runtime_apply(root,err,sizeof(err))==0){mlog("skin apply: ok id=%s",ev.msg);ui_theme_changed();ui_toast("下载完成，皮肤已即时切换");}else{mlog("skin apply: failed id=%s: %s",ev.msg,err[0]?err:"catalog root rejected");ui_toast(err[0]?err:"下载完成，应用失败，保留原界面");}break;}
        case EV_SEARCH_ENTITIES:
            if(ev.index<0){g_app.search_state=3;g_app.kb_edit=0;if(ev.msg[0])ui_toast(ev.msg);}
            else if(ev.index==g_app.search_kind){g_app.search_state=2;g_app.kb_edit=0;g_app.list_sel=0;g_app.list_scroll=0;}
            break;

        case EV_LIST_OK:
            g_app.list_from = g_app.screen;
            copy_list_from_api();
            if (g_app.screen == SC_SEARCH) {
                g_app.search_state = 2;
                g_app.kb_edit = 0;
                g_app.sug_focus = 0;
                g_app.list_sel = 0;
                g_app.list_scroll = 0;
                if (g_app.list_total > g_list.count)
                    ui_toast("找到结果，↓ 到底继续加载");
                else
                    ui_toast("找到结果，○ 播放");
            } else if (g_app.screen == SC_HOME && g_list_kind == LK_CHART) {
                /* 榜单在后台预加载好，人留在首页；点卡片直接进 */
                g_app.search_state = 0;
            } else {
                g_app.search_state = g_list_kind == LK_SEARCH ? 2 : 0;
                if (can_auto_navigate()) set_screen(SC_LIST);
            }
            if (g_list_kind != LK_RECENT && g_list_kind != LK_FAV) api_request_check();
            break;

        case EV_DAILY_OK:
        case EV_SONGS_OK:
            if (!search_return_accept()) break;
            if (g_app.screen != SC_LIST) g_app.list_from = g_app.screen;
            copy_list_from_api();
            g_app.list_sel = 0;
            g_app.list_scroll = 0;
            if (g_app.play_all_next) {
                g_app.play_all_next = 0;
                if (g_list.count > 0) { play_index(0); break; }
            }
            if (can_auto_navigate()) set_screen(SC_LIST);
            /* 歌单动辄上百首：只对短列表做可播性检查，避免 URL 过长/长时间卡住 */
            if (g_list.count > 0 && g_list.count <= 40) api_request_check();
            break;

        case EV_CARDS_OK:
            skin_transition_ready_section(g_app.screen,g_app.time); {
            const CardList *cl = g_app.grid_kind == 4 ? api_discovery_cards() : api_cards();

            for (int i = 0; i < cl->count && i < 12; ++i) api_thumb_slot(cl->items[i].cover);
            api_request_thumbs();

            if (!g_app.cards_open) {
                /* 后台预取/晚到的卡片：只存数据，绝不切屏。
                 * 只有用户按了「更多/推荐歌单」(cards_open=1) 才进网格。 */
                g_app.cards_stay = 0;
                g_app.cards_ready = 1;
                mlog("cards ready: %d (screen=%d, no jump)", cl->count, g_app.screen);
                break;
            }
            g_app.cards_open = 0;

            g_app.grid_sel = 0;
            g_app.grid_page = 0;
            g_app.grid_cols = (g_app.grid_kind == 4) ? 6 : 5;
            set_screen(g_app.grid_kind == 4 ? SC_SQUARE : SC_GRID);
            mlog("cards shown: %d (kind=%d)", cl->count, g_app.grid_kind);
            break;
        }

        case EV_COMMENTS_OK:
            if (ev.subject_id != api_comment_song() || ev.request_generation != api_comment_generation()) break;
            if (g_app.screen != SC_COMMENTS) break;
            g_app.cm_more = 0;
            if(ev.index!=1)g_app.cm_scroll=0; /* Appended comments preserve the reading position. */
            break;

        case EV_THUMB_OK:
            break;

        case EV_LIKE_OK:
            if (ev.subject_id && (ev.subject_id != g_like_subject ||
                ev.account_generation != api_account_generation() ||
                ev.account_generation != g_like_account_gen ||
                ev.request_generation != g_like_request_gen)) break;
            if (ev.msg[0]) ui_toast(ev.msg);
            if (ev.subject_id==g_app.now.id && ev.index >= 0 && ev.index < 2) g_app.is_liked = ev.index;
            break;
        case EV_LOGIN_OK:
            g_app.is_liked = 0;
            mlog("login ok: %s", api_login_name());
            api_request_liked_total();api_request_like_state();
            api_request_account();
            break;
        case EV_LIST_FAIL:
            skin_transition_ready_section(g_app.screen,g_app.time);
            g_search_return.pending = 0;
            g_search_return.valid = 0;
            if (ev.subject_id && ev.request_generation != api_comment_generation()) break;
            /* 任何一步失败都要把「等下一个事件」的标志清掉，否则翻页/加载更多会永久卡住 */
            g_app.list_more = 0;
            g_app.play_all_next = 0;
            g_app.cm_more = 0;
            g_app.cards_stay = 0;
            if (ev.msg[0]) ui_toast(ev.msg);
            if (g_app.screen == SC_SEARCH) g_app.search_state = 3;
            break;

        case EV_CHECK_DONE:
            for (int i = 0; i < g_list.count; ++i) {
                for (int j = 0; j < api_result()->count; ++j) {
                    if (g_list.songs[i].id == api_result()->songs[j].id) {
                        g_list.songs[i].playable = api_result()->songs[j].playable;
                        break;
                    }
                }
            }
            if ((dev_mode() == 2 || dev_mode() == 4 || dev_mode() == 8 || dev_mode() == 11 || dev_mode() == 12 || dev_mode() == 13 || dev_mode() == 14) && !g_autoplay &&
                (g_app.screen == SC_LIST || g_app.screen == SC_HOME)) {
                for (int i = 0; i < g_list.count; ++i) {
                    if (g_list.songs[i].playable != 2) { g_autoplay = 1; play_index(i); break; }
                }
            }
            /* Keep songs without an official URL in the list: they are played and
             * downloaded through the alternative sources. Dropping them here made
             * a Jay Chou search flash dozens of rows and then empty itself. */
            break;

        case EV_PLAY_OK:
            /* 旧请求（用户已经点了别的歌）的回应不能覆盖当前状态 */
            if (ev.index >= 0 && ev.index != g_app.playing_index) break;
            g_retry_waiting=0;
            g_app.paused = 0;
            g_auto_skip = 0;
            api_note_play_ok();
            g_fail_streak = 0;
            /* Do NOT clear g_retry_n here: the stream can report EV_PLAY_OK and
             * still fail on the first decode. It is cleared only after the song
             * has actually been playing for a couple of seconds (main loop). */
            g_retry_gave_up = 0;
            g_retry_t = 0;
            api_request_like_state();
            api_request_lyric(&g_app.now);
            if (g_play_fail > 0) g_play_fail = 0;
            break;

        case EV_PLAY_FAIL:
            if (ev.index >= 0 && ev.index != g_app.playing_index) break;
            g_retry_waiting=0;
            g_app.paused = 1;
            /* 有 MV 的歌不要自动跳走：VIP 歌经常只有 MV 能看，
             * 一跳走用户就再也点不到「播放 MV」了。 */
            if (g_app.now.mv > 0) {
                ui_toast("这首歌放不了，但可以看 MV：SELECT → 播放 MV");
                break;
            }
            if (ev.msg[0]) ui_toast(ev.msg);
            g_retry_gave_up=1;g_retry_t=0;audio_pause(1);
            if(!ev.msg[0])ui_toast("当前歌曲加载失败，已暂停；按○重试");
            break;

        case EV_ART_OK:
            break;

        case EV_LYRIC_OK:
            g_app.lyric_scroll = 0;
            break;

        case EV_DL_STARTED:
            ui_toast(ev.msg[0] ? "已加入下载" : "已加入下载");
            break;

        case EV_DL_FAIL:
            ui_toast(ev.msg[0] ? ev.msg : "完成");
            break;

        case EV_LIST_MORE: {
            const SongList *more = api_result();
            int i;

            g_app.list_more = 0;
            /* index 0 = 这一页没拿到（被打断/歌单换了/网络错），保持总数不变，
             * 用户再按一下还能重试；index 1 = 真的追加到了数据。 */
            if (ev.index == 0) {
                mlog("list more: no data");
                break;
            }
            for (i = 0; i < more->count && g_list.count < MAX_SONGS; ++i) {
                g_list.songs[g_list.count] = more->songs[i];
                g_list.songs[g_list.count].hot = 0;
                g_list.count++;
            }
            /* 服务端说还有，但这一页一首都没解析出来：把总数收回来，别无限重试 */
            if (i == 0) g_app.list_total = g_list.count;
            mlog("list more: +%d now %d/%d", i, g_list.count, g_app.list_total);
            if (g_list.count > 0 && g_list_kind == LK_SEARCH) api_request_check();
            break;
        }

        case EV_SUGGEST_OK:
            g_app.sug_state = (api_suggest_count() > 0) ? 2 : 0;
            if (g_app.sug_sel >= api_suggest_count()) g_app.sug_sel = 0;
            break;

        case EV_INFO:
            ui_toast(ev.msg[0] ? ev.msg : "完成");
            break;

        case EV_CACHE_DIRTY:
            break;

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 输入                                                                */
/* ------------------------------------------------------------------ */

static int btn_repeat(unsigned int pressed, unsigned int held, float dt, unsigned int mask)
{
    if (pressed & mask) {
        g_rep_btn = mask;
        g_rep_t = 0.36f;
        return 1;
    }
    if ((held & mask) && g_rep_btn == mask) {
        g_rep_t -= dt;
        if (g_rep_t <= 0) {
            g_rep_t = 0.08f;
            return 1;
        }
    }
    return 0;
}

static void adjust_volume(int delta)
{
    int v = audio_volume() + delta;

    if (v < 0) v = 0;
    if (v > 100) v = 100;
    audio_set_volume(v);
}

/* ---- 顶栏 tab：0 发现 / 1 我的 / 2 搜索 ---- */
static void switch_tab(int dir)
{
    g_app.tab = (g_app.tab + dir + 3) % 3;

    if (g_app.tab == 0) {
        set_screen(SC_HOME);
    } else if (g_app.tab == 1) {
        set_screen(SC_MINE);
        if (api_logged_in() && !api_user_avatar()[0]) api_request_user_detail();
    } else {
        g_app.tab = 2;
        if (g_app.search_state == 2) g_app.kb_edit = 0;
        else {
            g_app.kb_edit = 1;
            if (g_app.kb_row < -1) g_app.kb_row = 0;
        }
        set_screen(SC_SEARCH);
    }
}

static int home_row_of(int sel)
{
    if (sel <= HOME_ITEM_HERO) return 0;
    if (sel < HOME_ITEM_COVER) return 1;
    if (sel < HOME_ITEM_COVER + 5) return 2;
    if (sel < HOME_ITEM_QUICK) return 3;
    return 4;
}

static void home_row_range(int row, int *start, int *len)
{
    switch (row) {
    case 0: *start = HOME_ITEM_HERO; *len = 1; break;
    case 1: *start = HOME_ITEM_CHART; *len = 3; break;
    case 2: *start = HOME_ITEM_COVER; *len = 5; break;
    case 3: *start = HOME_ITEM_COVER + 5; *len = 5; break;
    default: *start = HOME_ITEM_QUICK; *len = HOME_QUICKS; break;
    }
}

/* ---- 首页 ---- */
static void input_home(unsigned int pressed, unsigned int held, float dt)
{
    int row = home_row_of(g_app.home_sel);
    int start, len;
    int moved = 0;

    home_row_range(row, &start, &len);

    if (btn_repeat(pressed, held, dt, PSP_CTRL_LEFT) && len > 1) {
        int col = g_app.home_sel - start;
        g_app.home_sel = start + (col + len - 1) % len;
        moved = 1;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_RIGHT) && len > 1) {
        int col = g_app.home_sel - start;
        g_app.home_sel = start + (col + 1) % len;
        moved = 1;
    }

    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        int nr = row > 0 ? row - 1 : 4;
        int col = g_app.home_sel - start;

        home_row_range(nr, &start, &len);
        if (col >= len) col = len - 1;
        g_app.home_sel = start + col;
        moved = 1;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        int nr = row < 4 ? row + 1 : 0;
        int col = g_app.home_sel - start;

        home_row_range(nr, &start, &len);
        if (col >= len) col = len - 1;
        g_app.home_sel = start + col;
        moved = 1;
    }

    g_app.tab = 0;

    if (pressed & PSP_CTRL_CIRCLE) {
        int sel = g_app.home_sel;

        if (sel == HOME_ITEM_HERO) {
            g_app.busy = 1;
            api_request_daily();
        } else if (sel < HOME_ITEM_COVER) {
            int ci = sel - HOME_ITEM_CHART;
            const Chart *c = api_chart(ci);

            if (c && g_list.count > 0 && g_list_kind == LK_CHART && g_list.chart_id == c->id) {
                g_app.list_from = SC_HOME;
                g_app.list_sel = 0;
                g_app.list_scroll = 0;
                set_screen(SC_LIST);
            } else if (c) {
                g_app.busy = 1;
                g_app.list_from = SC_HOME;
                api_request_chart(ci);
            }
        } else if (sel < HOME_ITEM_QUICK) {
            int idx = sel - HOME_ITEM_COVER;
            const CardList *cl = api_home_cards();

            if (idx < cl->count) api_request_playlist(cl->items[idx].id);
        } else {
            switch (sel - HOME_ITEM_QUICK) {
            case 0:
                /* 推荐歌单网格（首页「更多」的落点） */
                g_app.cards_stay = 0;
                g_app.grid_sel = 0;
                g_app.grid_page = 0;
                g_app.grid_kind = 0;
                if (api_cards()->count > 0) {
                    set_screen(SC_GRID);
                } else {
                    g_app.cards_open = 1;   /* 卡片到了再进网格 */
                    api_request_recommend_lists();
                }
                break;
            case 1:
                g_app.tab = 2;
                g_app.kb_edit = 1;
                g_app.search_state = 0;
                g_app.query[0] = 0;
                g_app.query_len = 0;
                g_app.kb_row = 0;
                g_app.kb_col = 0;
                set_screen(SC_SEARCH);
                break;
            case 2:
                g_app.dl_sel = 0;
                set_screen(SC_DOWNLOAD);
                break;
            case 3:
                g_app.cache_sel = 0;
                g_app.cache_tab = 0;
                set_screen(SC_CACHE);
                break;
            default:
                set_screen(SC_SETTINGS);
                break;
            }
        }
    }

    if (pressed & PSP_CTRL_LTRIGGER) switch_tab(-1);
    if (pressed & PSP_CTRL_RTRIGGER) switch_tab(1);

    if (pressed & PSP_CTRL_SQUARE) {
        if (store_recent_count() > 0) {
            g_list.count = 0;
            for (int i = 0; i < store_recent_count() && i < MAX_SONGS; ++i) {
                g_list.songs[i] = *store_recent(i);
                g_list.songs[i].playable = 0;
                g_list.songs[i].art[0] = 0;
                g_list.count++;
            }
            g_list.kind = LK_RECENT;
            snprintf(g_list.title, sizeof(g_list.title), "最近播放");
            g_list_title[0] = 0;
            g_list_kind = LK_RECENT;
            g_app.list_from = SC_HOME;
            g_app.list_sel = 0;
            g_app.list_scroll = 0;
            set_screen(SC_LIST);
        } else {
            ui_toast("还没有播放记录");
        }
    }

    if (pressed & PSP_CTRL_TRIANGLE) {
        if (g_app.has_now) set_screen(SC_PLAYER);
    }
    if (pressed & PSP_CTRL_SELECT) set_screen(SC_SETTINGS);

    (void)moved;
}

/* ---- 列表 ---- */
/* 列表续页：搜索走 offset 分页，歌单/我喜欢走 trackIds 分批详情。
 * 触发点比底部提前 10 行，滚到底时下一页通常已经回来了。 */
static void maybe_load_more(void)
{
    if (g_app.list_more) return;
    if (g_list.count >= MAX_SONGS) return;
    if (g_app.list_total <= g_list.count) return;

    if (g_list.kind == LK_PLAYLIST || g_list.kind == LK_LIKED) {
        if (g_app.list_sel >= g_list.count - 10) {
            g_app.list_more = 1;
            api_request_playlist_more(g_list.count);
        }
    } else if (g_list.kind == LK_SEARCH) {
        if (g_app.list_sel >= g_list.count - 6) {
            g_app.list_more = 1;
            api_request_search_more();
        }
    }
}

static void toggle_song_like(const Song*song){
    if(!song||song->id<=0)return;
    if(api_logged_in()){
        int liked=api_song_liked(song->id);
        if(liked<0){api_request_like_state();ui_toast("正在获取云端喜欢状态，请稍后再按□");return;}
        api_request_like(song->id,!liked);g_like_subject=song->id;g_like_account_gen=api_account_generation();g_like_request_gen=api_like_generation();
        ui_toast(liked?"正在取消云端喜欢":"正在同步到我喜欢的音乐");
    }else{int result=store_toggle_fav(song);ui_toast(result==2?"本地收藏已满":result==1?"已取消本地收藏":"已本地收藏，登录后可同步云端");}
}
static void toggle_playlist_favorite(const Card*card){
    int result=store_playlist_toggle(card);
    if(result<0){ui_toast("歌单收藏保存失败");return;}
    if(result==2){ui_toast("歌单收藏已满（24个）");return;}
    if(api_logged_in()){api_request_subscribe(card->id,result!=1);ui_toast(result==1?"本地已移除，正在取消云端收藏":"本地已收藏，正在同步云端歌单");}
    else ui_toast(result==1?"已取消本地歌单收藏":"已收藏歌单，可在我的页面查看");
}

static Song g_download_selected[32];
int app_download_selected(long long id){for(int i=0;i<g_app.download_selected_count;++i)if(g_download_selected[i].id==id)return 1;return 0;}
static void select_download_song(const Song*song){
 if(!song||song->id<=0)return;
 for(int i=0;i<g_app.download_selected_count;++i)if(g_download_selected[i].id==song->id){for(int j=i+1;j<g_app.download_selected_count;++j)g_download_selected[j-1]=g_download_selected[j];g_app.download_selected_count--;return;}
 if(g_app.download_selected_count>=32){ui_toast("一次最多选择32首");return;}g_download_selected[g_app.download_selected_count++]=*song;
}
static void submit_selected_downloads(void){
 int total=g_app.download_selected_count,added=0,kept=0;
 for(int i=0;i<total;++i){Song*song=&g_download_selected[i];int n=api_request_download_batch(song,1);if(n>0)added+=n;else if(!cache_has(song->id)&&!dl_has_song(song->id)&&!api_download_pending(song->id))g_download_selected[kept++]=*song;}
 g_app.download_selected_count=kept;g_app.download_select_mode=kept>0;char text[96];
 if(kept)snprintf(text,sizeof(text),"已加入%d首；队列满，%d首保留勾选待重试",added,kept);else snprintf(text,sizeof(text),"已加入%d首后台下载（重复或已缓存跳过）",added);ui_toast(text);
}

static void input_list(unsigned int pressed, unsigned int held, float dt)
{
    if(pressed&PSP_CTRL_LTRIGGER){g_app.download_select_mode=!g_app.download_select_mode;if(!g_app.download_select_mode)g_app.download_selected_count=0;ui_toast(g_app.download_select_mode?"多选下载：□选中，△开始，×取消":"已退出多选");return;}
    if(g_app.download_select_mode){
        if((pressed&PSP_CTRL_SQUARE)&&g_list.count>0)select_download_song(&g_list.songs[g_app.list_sel]);
        if(pressed&PSP_CTRL_TRIANGLE){submit_selected_downloads();return;}
        if(pressed&PSP_CTRL_CROSS){g_app.download_select_mode=0;g_app.download_selected_count=0;return;}
        pressed&=~(PSP_CTRL_SQUARE|PSP_CTRL_CIRCLE);
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        if (g_app.list_sel > 0) g_app.list_sel--;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        if (g_app.list_sel < g_list.count - 1) g_app.list_sel++;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_LEFT)) {
        g_app.list_sel -= 5;
        if (g_app.list_sel < 0) g_app.list_sel = 0;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_RIGHT)) {
        g_app.list_sel += 5;
        if (g_app.list_sel >= g_list.count) g_app.list_sel = g_list.count > 0 ? g_list.count - 1 : 0;
    }

    if (pressed & PSP_CTRL_CIRCLE) play_index(g_app.list_sel);
    if (pressed & PSP_CTRL_CROSS) {
        /* 回到来时的界面（首页 / 歌单网格 / 我的 / 搜索 / 缓存） */
        int from = g_app.list_from;

        if (from == SC_GRID || from == SC_SQUARE || from == SC_HOME ||
            from == SC_SEARCH || from == SC_MINE || from == SC_CACHE)
            set_screen(from);
        else
            set_screen(SC_HOME);
    }

    if (pressed & PSP_CTRL_TRIANGLE) download_current();

    if (pressed & PSP_CTRL_SQUARE) {
        if (g_list.count > 0) {
            toggle_song_like(&g_list.songs[g_app.list_sel]);
        }
    }

    if (pressed & PSP_CTRL_SELECT) {
        if(g_list_kind==LK_PLAYLIST&&g_list.chart_id>0){Card card={0};card.id=g_list.chart_id;snprintf(card.name,sizeof(card.name),"%s",g_list.title);toggle_playlist_favorite(&card);}
        else if (g_app.has_now) set_screen(SC_PLAYER);
    }
}

/* ---- 播放页 ---- */
static int g_mv_restore_pause;
static int g_mv_from = SC_PLAYER;
static int g_mv_pending;          /* 正在等下载完成的 MV id */
static unsigned g_mv_connect_started;
static int g_mv_connect_native;
static int g_mv_connect_drawn;
static int g_mv_connecting;       /* SC_MV without a decoder thread yet */
/* Faulted join can hide a retained worker from both MV query APIs.
 * Keep its UI/music ownership until process restart, just like its resources. */
static int g_mv_ui_retained;
static void mv_connect_step(const char*tag){
 int fd=sceIoOpen(APP_DIR "mv-connect.txt",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND,0777);
 if(fd<0)return;
 unsigned n=0;while(tag[n])n++;
 sceIoWrite(fd,tag,n);sceIoWrite(fd,"\n",1);sceIoClose(fd);sceIoSync("ms0:",0);
}


static void mv_connect_cancel(void)
{
    api_mv_download_abort();
    g_mv_pending = 0;
    g_mv_connecting = 0;
    if(!mv_active()){skin_runtime_resume();ui_theme_changed();}
    audio_pause(g_mv_restore_pause);
    set_screen(g_mv_from);
}

static int mv_try_start(void)
{
    char path[128];
    SceIoStat st;

    if(mvx_active()){ui_toast("MV正在导出，请完成后再播放");return 0;}
    if(mv_stream_worker_active()&&api_mv_info()->id!=g_app.now.mv){ui_toast("另一支MV正在下载，请先完成或取消");return 0;}
    mlog("mv_try: has=%d now_mv=%d", g_app.has_now, g_app.now.mv);
    if (!g_app.has_now || g_app.now.mv <= 0) {
        ui_toast("这首歌没有 MV");
        return 0;
    }
    snprintf(path, sizeof(path), "./cache/mv_%d_240.mp4",
             g_app.now.mv);
    if (g_mv_connecting) return 1;
    g_mv_restore_pause = audio_paused();
    g_mv_from = g_app.screen;
    audio_pause(1);
    g_app.menu_open = 0;
    if (sceIoGetstat(path, &st) < 0) {
        g_mv_pending = g_app.now.mv;
        g_mv_connecting = 1;
        g_mv_connect_started=sceKernelGetSystemTimeLow();g_mv_connect_drawn=0;
        g_mv_connect_native=mv_native_requested();
        mv_connect_step("before-request");
        api_request_mv_download(g_app.now.mv);
        mv_connect_step("after-request");
        set_screen(SC_MV);
        mv_connect_step("entered-connecting-page");
        return 1;
    }
    g_mv_pending = 0;
    if (mv_start(g_app.now.mv, path, g_app.now.name) == 0) {
        g_app.menu_open = 0;
        set_screen(SC_MV);
        return 1;
    }
    if(!mv_active()){skin_runtime_resume();ui_theme_changed();}
    audio_pause(g_mv_restore_pause);
    ui_toast(mv_block_reason() == 1 ? "内存不足：请重启后再看 MV" : mv_block_reason()==2?"解码器初始化失败，请先尝试软件解码":"MV 启动失败");
    return 0;
}

/* URL resolution stays on the API worker until the independent source worker
 * has data. Only then may mv_begin release the API worker and its buffers. */
static void mv_pending_tick(void)
{
    int st, id, streaming = 0;
    char path[160];
    long long av = 0;

    if (g_mv_pending <= 0) return;
    if (!g_app.has_now || g_app.now.mv != g_mv_pending ||
        (g_app.screen != SC_PLAYER &&
         !(g_app.screen == SC_MV && g_mv_connecting))) {
        if (g_mv_connecting) mv_connect_cancel();
        else { api_mv_download_abort(); g_mv_pending = 0; }
        return;
    }
    if(g_mv_connecting && (unsigned)(sceKernelGetSystemTimeLow()-g_mv_connect_started)>=30000000u && (mv_stream_active()?mv_stream_total()==0:api_mv_download_total()==0)){
        mv_connect_step("connecting-deadline-30s");mv_connect_cancel();ui_toast("视频连接超时，已取消，请重试");return;
    }
    st = api_mv_download_state();
    if(g_mv_connecting){static int previous_state=-1;if(st!=previous_state){
        mv_connect_step(st==0?"request-queued":st==1?"request-running":st==2?"download-complete":"request-failed");previous_state=st;
    }}
    if (st == 0) return; /* API request is queued, URL resolution not started yet. */
    if (st == 3) {
        if (g_mv_connecting) mv_connect_cancel();
        else g_mv_pending = 0;
        g_retry_gave_up=1;g_retry_t=0;g_app.paused=1;audio_pause(1);
        ui_toast("MV连接失败，已暂停当前歌曲；请检查网络后重试");
        return;
    }
    if (st == 1 && mv_stream_active()) {
        long long need = mv_stream_prefetch();
        av = mv_stream_available();
        if (av < need) return;
        /* Buffer alone is not enough: a link slower than the video bitrate plays a
         * few seconds, then waits again — reported as "MV 总是卡". Keep downloading
         * and let the connecting page show the honest rate until it completes. */
        if (!mv_stream_sustainable() && av < mv_stream_total()) return;
        streaming = 1;
    } else if (st != 2) {
        return;
    }
    id = g_mv_pending;
    g_mv_pending = 0;
    snprintf(path, sizeof(path), APP_DIR "cache/mv_%d_240.mp4", id);
    /* Legacy diagnostic pending requests may still originate on SC_PLAYER. */
    if (!g_mv_connecting) {
        g_mv_restore_pause = audio_paused();
        g_mv_from = g_app.screen;
        audio_pause(1);
    }
    if ((streaming ? mv_start_stream(path, g_app.now.name) :
                     mv_start(id, path, g_app.now.name)) == 0) {
        mlog("mv: pending start stream=%d at %lld/%lld", streaming, av, mv_stream_total());
        g_mv_connecting = 0;
        g_app.menu_open = 0;
        set_screen(SC_MV);
    } else {
        mv_connect_cancel();
        ui_toast(mv_block_reason() == 1 ? "内存不足：请重启后再看 MV" : mv_block_reason()==2?"解码器初始化失败，请先尝试软件解码":"MV 启动失败");
    }
}

static void player_menu_act(void)
{
    switch (g_app.menu_sel) {
    case 0:
        g_app.menu_open = 0;
        g_app.cm_scroll = 0;
        g_app.comment_from = SC_PLAYER;
        if (g_app.has_now) { set_screen(SC_COMMENTS); api_request_comments(g_app.now.id, 0); }
        break;
    case 1:
        g_app.play_mode = (g_app.play_mode + 1) % 3;
        ui_toast(g_app.play_mode == 0 ? "顺序播放"
                 : (g_app.play_mode == 1 ? "随机播放" : "单曲循环"));
        break;
    case 2:
        g_app.quality = (g_app.quality + 1) % 3;
        store_set_quality(g_app.quality);
        ui_toast(g_app.quality == 0 ? "音质：标准 128k"
                 : (g_app.quality == 1 ? "音质：较高 192k" : "音质：极高 320k"));
        break;
    case 3:
        g_app.menu_open = 0;
        g_app.lyrics_open = 1;
        g_app.lyric_scroll = 0;
        set_screen(SC_LYRICS);
        break;
    case 4:
        g_app.menu_open = 0;
        set_screen(g_app.play_from ? g_app.play_from : SC_LIST);
        break;
    case 6:
        mv_try_start();
        break;
    case 7:
        g_app.menu_open=0;ui_toast(api_request_mv_save(g_app.now.mv)==0?"MV已加入下载，进度见下载页":"已有MV任务，请先完成或取消");break;
    case 8:
        g_app.menu_open=0;ui_toast(mvx_start(g_app.now.mv,g_app.now.name,g_app.now.artist)==0?"正在导出MV到 VIDEO":"导出未启动：请先下载完整MV");break;
    default:
        g_app.menu_open = 0;
        break;
    }
}

static void playback_direction_input(unsigned int pressed,unsigned int held,float dt)
{
    /* ↑↓ 音量，←→ 快退/快进，L/R 切歌 */
    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) adjust_volume(10);
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) adjust_volume(-10);
    if (btn_repeat(pressed, held, dt, PSP_CTRL_LEFT)) {
        audio_seek(audio_pos_ms() - 15000);
        g_seek_guard_t = g_app.time;
        g_auto_fix_n = 0;
        g_auto_fix_t = g_app.time;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_RIGHT)) {
        audio_seek(audio_pos_ms() + 15000);
        g_seek_guard_t = g_app.time;
        g_auto_fix_n = 0;
        g_auto_fix_t = g_app.time;
    }
}

static void input_player(unsigned int pressed, unsigned int held, float dt)
{
    if (g_app.menu_open) {
        if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
            if (g_app.menu_sel > 0) g_app.menu_sel--;
        }
        if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
            if (g_app.menu_sel < (g_app.now.mv > 0 ? 8 : 5)) g_app.menu_sel++;
        }
        if (pressed & PSP_CTRL_CIRCLE) player_menu_act();
        if (pressed & PSP_CTRL_CROSS) g_app.menu_open = 0;
        return;
    }

    if (pressed & PSP_CTRL_SELECT) {
        g_app.menu_open = 1;
        g_app.menu_sel = 0;
        return;
    }

    if (pressed & PSP_CTRL_CIRCLE) {
        if(g_retry_gave_up||audio_state()==AUDIO_ERROR){g_retry_gave_up=0;g_retry_n=0;g_retry_t=0;g_retry_waiting=1;g_app.paused=0;api_request_play(g_app.playing_index,&g_app.now);}
        else {g_app.paused = !g_app.paused;audio_pause(g_app.paused);}
    }

    if (pressed & PSP_CTRL_CROSS) {
        /* 回到来时的界面：搜索 / 缓存（含本地标签） / 榜单 */
        if (g_app.play_from == SC_SEARCH) {
            set_screen(SC_SEARCH);
        } else if (g_app.play_from == SC_CACHE) {
            g_app.cache_tab = g_app.play_from_tab;
            set_screen(SC_CACHE);
        } else {
            set_screen(SC_LIST);
        }
    }

    if (pressed & PSP_CTRL_TRIANGLE) {
        g_app.lyrics_open = 1;
        g_app.lyric_scroll = 0;
        set_screen(SC_LYRICS);
    }

    playback_direction_input(pressed,held,dt);
    if (pressed & PSP_CTRL_LTRIGGER) {
        g_auto_skip = 0;
        play_prev();
    }
    if (pressed & PSP_CTRL_RTRIGGER) {
        g_auto_skip = 0;
        play_next(1);
    }

    if (pressed & PSP_CTRL_SQUARE)toggle_song_like(&g_app.now);

    if (pressed & PSP_CTRL_DOWN) {
        /* ↓ 是音量减（在上面），评论统一走 SELECT → 看评论 */
    }
}

/* ---- 歌词 ---- */
static void input_lyrics(unsigned int pressed, unsigned int held, float dt)
{
    if ((pressed & PSP_CTRL_CROSS) || (pressed & PSP_CTRL_TRIANGLE)) {
        g_app.lyrics_open = 0;
        set_screen(SC_PLAYER);
    }
    if (pressed & PSP_CTRL_CIRCLE) {
        g_app.paused = !g_app.paused;
        audio_pause(g_app.paused);
    }
    playback_direction_input(pressed,held,dt);
    if(pressed&PSP_CTRL_SQUARE){g_app.lyric_translation=!g_app.lyric_translation;ui_toast(g_app.lyric_translation?(api_lyric()->translated_count?"歌词翻译已开启":"已开启翻译，此歌曲暂无翻译"):"歌词翻译已关闭");}

    /* L/R switches songs; directions match the player page. */
    if (pressed & PSP_CTRL_LTRIGGER) {
        g_auto_skip = 0;
        play_prev();
    }
    if (pressed & PSP_CTRL_RTRIGGER) {
        g_auto_skip = 0;
        play_next(1);
    }
}

static void open_skin_center(void);
static void input_settings(unsigned pressed,unsigned held,float dt);
static void input_mine(unsigned pressed,unsigned held,float dt);
static void input_cache(unsigned pressed,unsigned held,float dt);
static void skin_actions_tick(void){int notice=skin_runtime_take_notice();if(notice==1)ui_toast("皮肤该页面出错，暂用原界面；其他页面保留");else if(notice==2)ui_toast("皮肤内存不足，已回退；可重新应用皮肤");char action[48],arg[128];if(!skin_runtime_action(action,sizeof(action),arg,sizeof(arg)))return;
 if(!strcmp(action,"setting")||!strcmp(action,"setting_adjust")){int index=atoi(arg)-1;if(index>=0&&index<MELODY_SETTINGS_COUNT){g_app.settings_sel=index;input_settings(!strcmp(action,"setting_adjust")?PSP_CTRL_RIGHT:PSP_CTRL_CIRCLE,0,.016f);}return;}
 if(!strcmp(action,"cancel_skin_load")){g_app.cards_open=0;g_app.play_all_next=0;g_app.busy=0;return;}
 if(!strcmp(action,"xmb_back")){xmb_go_back();return;}
 if(!strcmp(action,"xmb_open")){
  if(!xmb_active())return;
  if(!strncmp(arg,"downloads:",10)){int tab=atoi(arg+10);if(tab>=0&&tab<=1){set_screen(SC_DOWNLOAD);g_app.dl_tab=tab;g_app.dl_sel=0;}return;}
  if(!strncmp(arg,"cache:",6)){int tab=atoi(arg+6);if(tab>=0&&tab<=1){set_screen(SC_CACHE);g_app.cache_tab=tab;if(tab){g_app.local_sel=0;local_rescan();}else g_app.cache_sel=0;}return;}
  if(!strncmp(arg,"search:",7)){int kind=atoi(arg+7);if(kind>=0&&kind<5){set_screen(SC_SEARCH);g_app.search_kind=kind;g_app.kb_edit=1;g_app.list_sel=0;}return;}
  if(!strncmp(arg,"discovery:",10)){int cat=atoi(arg+10);if(cat>=-1&&cat<8){g_app.cat_sel=cat;g_app.grid_kind=4;g_app.grid_sel=g_app.grid_page=0;g_app.cards_stay=0;g_app.cards_open=1;api_request_discovery(cat);}return;}
  return;
 }
 if(!strcmp(action,"category_select")){int i=atoi(arg);if(i>=-1&&i<8){skin_transition_wait_section(g_app.screen,77,232,g_app.time);g_app.cat_sel=i;g_app.grid_kind=4;g_app.grid_sel=g_app.grid_page=0;g_app.cards_stay=1;g_app.cards_open=1;api_request_discovery(i);}return;}
 if(!strcmp(action,"grid_select")){int i=atoi(arg)-1;const CardList*cl=g_app.screen==SC_SQUARE?api_discovery_cards():api_cards();if(i>=0&&i<cl->count){g_app.grid_page=i/12;g_app.grid_sel=i%12;}return;}
 if(!strcmp(action,"mine_focus")){int i=atoi(arg)-1;if(i>=0&&i<12)g_app.mine_sel=i;return;}
 if(!strcmp(action,"mine")){g_app.mine_sel=7;set_screen(SC_MINE);return;}
 if(!strcmp(action,"mine_select")){int i=atoi(arg)-1;if(i>=0&&i<12){g_app.mine_sel=i;input_mine(PSP_CTRL_CIRCLE,0,.016f);}return;}
 if(!strcmp(action,"toggle_translation")){g_app.lyric_translation=!g_app.lyric_translation;ui_toast(g_app.lyric_translation?(api_lyric()->translated_count?"歌词翻译已开启":"已开启翻译，此歌曲暂无翻译"):"歌词翻译已关闭");return;}
 if(!strcmp(action,"daily")){api_request_daily();return;}
 if(!strcmp(action,"square")){g_app.cards_stay=0;g_app.cards_open=1;g_app.grid_kind=4;g_app.grid_sel=g_app.grid_page=0;g_app.cat_sel=-1;g_app.cat_focus=1;api_request_discovery(-1);return;}
 if(!strcmp(action,"local")){g_app.cache_tab=1;g_app.local_sel=0;local_rescan();set_screen(SC_CACHE);return;}
 if(!strcmp(action,"cache")){g_app.cache_tab=0;g_app.cache_sel=0;set_screen(SC_CACHE);return;}
 if(!strcmp(action,"cache_select")){int i=atoi(arg)-1;if(i>=0&&i<cache_count())g_app.cache_sel=i;return;}
 if(!strcmp(action,"cache_play")){int i=atoi(arg)-1;if(i>=0&&i<cache_count())play_cache_entry(i);return;}
 if(!strcmp(action,"favorites")){g_app.mine_sel=0;input_mine(PSP_CTRL_CIRCLE,0,.016f);return;}
 if(!strcmp(action,"recent")){g_app.mine_sel=1;input_mine(PSP_CTRL_CIRCLE,0,.016f);return;}
 if(!strcmp(action,"my_playlists")){g_app.mine_sel=2;input_mine(PSP_CTRL_CIRCLE,0,.016f);return;}
 if(!strcmp(action,"liked_songs")){api_request_liked();return;}
 if(!strcmp(action,"search_open")){long long id=atoll(arg);const CardList*results=api_search_entities();if(g_app.screen==SC_SEARCH&&g_app.search_kind>0&&g_app.search_kind<5)for(int i=0;i<results->count;++i)if(results->items[i].id==id){g_app.list_sel=i;search_return_save();g_app.list_from=SC_SEARCH;api_request_search_open(g_app.search_kind,id);break;}return;}
 if(!strcmp(action,"search_edit")){set_screen(SC_SEARCH);g_app.kb_edit=1;return;}
 if(!strcmp(action,"search_kind")){int kind=atoi(arg);if(kind>=0&&kind<5){g_app.search_kind=kind;g_app.kb_edit=0;g_app.search_state=1;g_app.list_sel=0;api_request_search_category(g_app.query,kind);}return;}
 if(!strcmp(action,"next")){play_next(1);return;}if(!strcmp(action,"previous")){play_prev();return;}
 if(!strcmp(action,"pause")){g_app.paused=!g_app.paused;audio_pause(g_app.paused);return;}
 if(!strcmp(action,"volume")){adjust_volume(atoi(arg));return;}if(!strcmp(action,"seek")){audio_seek(audio_pos_ms()+atoi(arg));g_seek_guard_t=g_app.time;return;}
 if(!strcmp(action,"select_song")){play_index(atoi(arg)-1);return;}if(!strcmp(action,"open_playlist")){if(g_app.screen==SC_SQUARE||g_app.screen==SC_GRID)g_app.list_from=g_app.screen;api_request_playlist(atoll(arg));return;}if(!strcmp(action,"open_artist")){if(g_app.screen==SC_SQUARE||g_app.screen==SC_GRID)g_app.list_from=g_app.screen;api_request_artist_songs(atoll(arg),"歌手歌曲");return;}
 if(!strcmp(action,"download_song")){int i=atoi(arg)-1;if(i>=0&&i<g_list.count)api_request_download(&g_list.songs[i]);return;}if(!strcmp(action,"like")){if(g_app.has_now)toggle_song_like(&g_app.now);return;}
 if(!strcmp(action,"comments")){if(g_app.has_now){g_app.comment_from=g_app.screen;g_app.cm_scroll=0;set_screen(SC_COMMENTS);api_request_comments(g_app.now.id,0);}return;}
 if(!strcmp(action,"search_query")){snprintf(g_app.query,sizeof(g_app.query),"%s",arg);g_app.query_len=strlen(g_app.query);g_app.kb_edit=0;g_app.search_state=1;set_screen(SC_SEARCH);api_request_search_category(arg,g_app.search_kind);return;}
 if(!strcmp(action,"skins")){open_skin_center();return;}if(!strcmp(action,"refresh_skins")){api_request_skin(0,NULL);return;}
 const char*names[]={"home","mine","search","songs","player","lyrics","downloads","cache","settings","about","account","square","sources"};const int pages[]={SC_HOME,SC_MINE,SC_SEARCH,SC_LIST,SC_PLAYER,SC_LYRICS,SC_DOWNLOAD,SC_CACHE,SC_SETTINGS,SC_HELP,SC_LOGIN,SC_SQUARE,SC_ALT};
 for(unsigned i=0;i<sizeof(pages)/sizeof(pages[0]);++i)if(!strcmp(action,names[i])){set_screen(pages[i]);if(pages[i]==SC_SEARCH)g_app.kb_edit=1;if(pages[i]==SC_LOGIN)g_app.login_from=SC_HOME;return;}
}

/* ---- 搜索 ---- */
static int kb_cols(int row)
{
    if (row == 0 || row == 1) return 10;
    return 9;
}

static void search_refresh_chips(void);

static void search_do(const char *q)
{
    if (!q[0]) { ui_toast("先输入拼音"); return; }
    if (search_hist_push(q)) store_save();
    search_refresh_chips();

    g_app.search_state = 1;
    g_app.kb_edit = 0;
    g_app.list_sel = 0;
    g_app.list_scroll = 0;
    g_app.busy = 1;
    api_request_search_category(q,g_app.search_kind);

    {
        int exists = 0;
        for (int i = 0; i < g_app.hist_n; ++i)
            if (!strcmp(g_app.hist[i], q)) exists = 1;
        if (!exists && g_app.hist_n < 6) {
            snprintf(g_app.hist[g_app.hist_n], sizeof(g_app.hist[0]), "%s", q);
            g_app.hist_n++;
        }
    }
}

/* 推荐搜索：优先最近播放的歌手，其次收藏，最后内置兜底。
 * 结果写进 g_app.chips，ui.c 只读。 */
static void search_refresh_chips(void)
{
    const char *recent[APP_CHIP_MAX * 2];
    const char *fav[APP_CHIP_MAX * 2];
    int rn = 0, fn = 0;

    for (int i = 0; i < store_recent_count() && rn < (int)(sizeof(recent) / sizeof(recent[0])); ++i) {
        const Song *s = store_recent_get(i);

        if (s && s->artist[0]) recent[rn++] = s->artist;
    }
    for (int i = 0; i < store_fav_count() && fn < (int)(sizeof(fav) / sizeof(fav[0])); ++i) {
        const Song *s = store_fav(i);

        if (s && s->artist[0]) fav[fn++] = s->artist;
    }
    g_app.chips_n = search_chips_build(recent, rn, fav, fn, g_app.chips, APP_CHIP_MAX);
    if (g_app.chip_sel >= g_app.chips_n) g_app.chip_sel = 0;
}

static void query_append(const char *s)
{
    int len = (int)strlen(g_app.query);
    int add = (int)strlen(s);

    if (len + add >= (int)sizeof(g_app.query) - 1) return;
    memcpy(g_app.query + len, s, (size_t)add + 1);
    g_app.query_len = len + add;
}

static void query_backspace(void)
{
    int len = (int)strlen(g_app.query);

    if (len <= 0) return;
    /* UTF-8 安全删除一个字符 */
    len--;
    while (len > 0 && ((unsigned char)g_app.query[len] & 0xC0) == 0x80) len--;
    g_app.query[len] = 0;
    g_app.query_len = len;
}

/* 字母进输入法：ASCII 模式直接写搜索框，拼音/假名先进 preedit */
/* 上屏后立即刷新联想：本地词库补全 + 在线联想（提示词也算） */
static void search_after_commit(void)
{
    g_app.assoc_n = ime_assoc(g_app.query, g_app.assoc, APP_CHIP_MAX);
    g_app.chip_sel = 0;
    if (g_app.query[0]) {
        snprintf(g_app.sug_last, sizeof(g_app.sug_last), "%s", g_app.query);
        g_app.sug_state = 1;
        g_app.sug_sel = 0;
        api_request_suggest(g_app.query);
    }
}

static void search_type_char(char ch)
{
    static char last_pre[24];
    int r = ime_feed(ch);

    if (r == 2) {
        char s[2] = { ch, 0 };

        query_append(s);
    } else if (r == 1) {
        query_append(ime_last_commit());
    }
    if (ime_active() && ime_candidate_count() == 0 && strcmp(last_pre, ime_preedit())) {
        snprintf(last_pre, sizeof(last_pre), "%s", ime_preedit());
        mlog("ime: no candidate for [%s] mode=%d", ime_preedit(), ime_mode());
    }
}

static void input_search(unsigned int pressed, unsigned int held, float dt)
{
    static const char *kb0 = "qwertyuiop";
    static const char *kb1 = "asdfghjkl";
    static const char *kb2 = "zxcvbnm";

    g_app.tab = 2;

    /* 结果态才用 L/R 切分类；输入态留给输入法选词 */
    if(!g_app.kb_edit && pressed&(PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER)){
        int direction=(pressed&PSP_CTRL_RTRIGGER)?1:4;g_app.search_kind=(g_app.search_kind+direction)%5;g_app.list_sel=0;g_app.list_scroll=0;
        if(g_app.query[0])search_do(g_app.query);else ui_toast(api_search_category_name(g_app.search_kind));return;
    }
    if(!g_app.kb_edit&&g_app.search_kind){
        const CardList*results=api_search_entities();
        if(btn_repeat(pressed,held,dt,PSP_CTRL_UP)&&g_app.list_sel>0)--g_app.list_sel;
        if(btn_repeat(pressed,held,dt,PSP_CTRL_DOWN)&&g_app.list_sel+1<results->count)++g_app.list_sel;
        if(btn_repeat(pressed,held,dt,PSP_CTRL_LEFT)){g_app.list_sel-=4;if(g_app.list_sel<0)g_app.list_sel=0;}
        if(btn_repeat(pressed,held,dt,PSP_CTRL_RIGHT)){g_app.list_sel+=4;if(g_app.list_sel>=results->count)g_app.list_sel=results->count?results->count-1:0;}
        if((pressed&PSP_CTRL_CIRCLE)&&g_app.list_sel>=0&&g_app.list_sel<results->count){search_return_save();g_app.list_from=SC_SEARCH;api_request_search_open(g_app.search_kind,results->items[g_app.list_sel].id);}
        if(pressed&PSP_CTRL_TRIANGLE)g_app.kb_edit=1;
        if(pressed&PSP_CTRL_CROSS)set_screen(SC_HOME);
        return;
    }

    if (!g_app.kb_edit) {
        if(pressed&PSP_CTRL_SELECT){g_app.download_select_mode=!g_app.download_select_mode;if(!g_app.download_select_mode)g_app.download_selected_count=0;ui_toast(g_app.download_select_mode?"多选下载：□选中，△开始，×取消":"已退出多选");return;}
        if(g_app.download_select_mode){
            if((pressed&PSP_CTRL_SQUARE)&&g_list.count>0)select_download_song(&g_list.songs[g_app.list_sel]);
            if(pressed&PSP_CTRL_TRIANGLE){submit_selected_downloads();return;}
            if(pressed&PSP_CTRL_CROSS){g_app.download_select_mode=0;g_app.download_selected_count=0;return;}
            pressed&=~(PSP_CTRL_SQUARE|PSP_CTRL_CIRCLE);
        }
        /* 结果列表 */
        if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
            if (g_app.list_sel > 0) g_app.list_sel--;
        }
        if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
            if (g_app.list_sel < g_list.count - 1) g_app.list_sel++;
        }
        if(btn_repeat(pressed,held,dt,PSP_CTRL_LEFT)){g_app.list_sel-=5;if(g_app.list_sel<0)g_app.list_sel=0;}
        if(btn_repeat(pressed,held,dt,PSP_CTRL_RIGHT)){g_app.list_sel+=5;if(g_app.list_sel>=g_list.count)g_app.list_sel=g_list.count?g_list.count-1:0;}
        if (pressed & PSP_CTRL_CIRCLE) play_index(g_app.list_sel);
        if (pressed & PSP_CTRL_SQUARE) download_current();
        if (pressed & PSP_CTRL_TRIANGLE) {
            g_app.kb_edit = 1;
            g_app.sug_focus = 0;
            g_app.sug_t = 0;
            ime_reset();
        }
        if (pressed & PSP_CTRL_CROSS) set_screen(SC_HOME);
        if (pressed & PSP_CTRL_SELECT) {
            g_app.query[0] = 0;
            g_app.query_len = 0;
            g_app.kb_edit = 1;
            ime_reset();
        }
        return;
    }

    /* ---------------- 输入模式 ---------------- */
    if (g_app.chips_n == 0) search_refresh_chips();

    if (pressed & PSP_CTRL_SQUARE) {
        ime_toggle_mode();
        ui_toast(ime_mode() == IME_KANA ? "输入法：假名（罗马字）" : ime_mode() == IME_ASCII ? "输入法：英文直输" : "输入法：拼音");
        g_app.ime_sel = 0;
        return;
    }
    if (pressed & PSP_CTRL_SELECT) {
        g_app.query[0] = 0;
        g_app.query_len = 0;
        ime_reset();
        g_app.ime_sel = 0;
        return;
    }
    if (pressed & PSP_CTRL_CROSS) {
        ime_reset();
        set_screen(SC_HOME);
        return;
    }
    if (pressed & PSP_CTRL_TRIANGLE) {
        /* △：有字库候选就跳到候选行，没有候选就跳到联想行；删字用 ← */
        int bot_n = ime_active() ? ime_candidate_count() : 0;
        int sug_n = api_suggest_count();
        int top_n = g_app.assoc_n > 0 ? g_app.assoc_n
                  : (sug_n > 0 ? sug_n : (ime_active() ? 0 : g_app.chips_n));

        if (bot_n > 0) {
            g_app.kb_row = -2;
            g_app.ime_sel = 0;
            g_app.ime_off = 0;
        } else if (top_n > 0) {
            g_app.kb_row = -1;
            g_app.chip_sel = 0;
            g_app.chip_off = 0;
        }
        return;
    }

    /* 联想：输入变化 0.45 秒后才请求；输入法还在拼写时不请求，避免和候选抢行 */
    if (!ime_active() && !g_app.sug_focus && strcmp(g_app.query, g_app.sug_last)) {
        g_app.sug_t += dt;
        if (g_app.sug_t > 0.45f) {
            g_app.sug_t = 0;
            snprintf(g_app.sug_last, sizeof(g_app.sug_last), "%s", g_app.query);
            if (g_app.query[0]) {
                g_app.sug_state = 1;
                g_app.sug_sel = 0;
                api_request_suggest(g_app.query);
            } else {
                g_app.sug_state = 0;
            }
        }
    }

    /* 焦点行：-1 = 上排（联想/推荐），-2 = 下排（候选），-3 = 右列历史；空行跳过 */
    {
        int ime = ime_active();
        int bot_n = ime ? ime_candidate_count() : 0;
        int sug_n = api_suggest_count();
        int top_n = g_app.assoc_n > 0 ? g_app.assoc_n
                  : (sug_n > 0 ? sug_n : (ime ? 0 : g_app.chips_n));
        int hist_n = search_hist_count();
        int row = g_app.kb_row;

        if (g_app.chip_sel >= top_n) g_app.chip_sel = top_n ? top_n - 1 : 0;
        if (g_app.ime_sel >= bot_n) g_app.ime_sel = bot_n ? bot_n - 1 : 0;
        if (g_app.hist_sel >= hist_n) g_app.hist_sel = hist_n ? hist_n - 1 : 0;

        if (row <= -1) {
            if (btn_repeat(pressed, held, dt, PSP_CTRL_LEFT)) {
                if (row == -3) {
                    g_app.kb_row = top_n > 0 ? -1 : (bot_n > 0 ? -2 : 0);
                } else if (row == -1 && g_app.chip_sel > 0) {
                    g_app.chip_sel--;
                    if (g_app.chip_sel < g_app.chip_off) g_app.chip_off = g_app.chip_sel;
                } else if (row == -2 && g_app.ime_sel > 0) {
                    g_app.ime_sel--;
                    if (g_app.ime_sel < g_app.ime_off) g_app.ime_off = g_app.ime_sel;
                }
            }
            if (btn_repeat(pressed, held, dt, PSP_CTRL_RIGHT)) {
                if (row == -1) {
                    if (g_app.chip_sel + 1 < top_n) {
                        g_app.chip_sel++;
                        if (g_app.chip_sel >= g_app.chip_off + 3) g_app.chip_off = g_app.chip_sel - 2;
                    } else if (hist_n > 0) {
                        g_app.kb_row = -3;      /* 上排最右再右移 → 右侧历史列 */
                        g_app.hist_sel = 0;
                    }
                } else if (row == -2 && g_app.ime_sel + 1 < bot_n) {
                    g_app.ime_sel++;
                    if (g_app.ime_sel >= g_app.ime_off + 4) g_app.ime_off = g_app.ime_sel - 3;
                }
            }
            if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
                if (row == -3) {
                    if (g_app.hist_sel > 0) g_app.hist_sel--;
                } else if (row == -2) {
                    if (top_n > 0) g_app.kb_row = -1;
                    else if (hist_n > 0) g_app.kb_row = -3;
                } else if (hist_n > 0) {
                    g_app.kb_row = -3;
                }
            }
            if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
                if (row == -3) {
                    if (g_app.hist_sel + 1 < hist_n) g_app.hist_sel++;
                    else g_app.kb_row = top_n > 0 ? -1 : (bot_n > 0 ? -2 : 0);
                } else if (row == -1) {
                    if (bot_n > 0) g_app.kb_row = -2;
                    else { g_app.kb_row = 0; if (g_app.kb_col > 9) g_app.kb_col = 9; }
                } else {
                    g_app.kb_row = 0;
                    if (g_app.kb_col > 9) g_app.kb_col = 9;
                }
            }
            if (pressed & PSP_CTRL_CIRCLE) {
                if (row == -3) {
                    const char *q = search_hist_at(g_app.hist_sel);

                    if (q[0]) {
                        snprintf(g_app.query, sizeof(g_app.query), "%s", q);
                        g_app.query_len = (int)strlen(g_app.query);
                        search_do(g_app.query);
                        search_after_commit();
                    }
                } else if (row == -1 && top_n > 0) {
                    const char *txt = g_app.assoc_n > 0 ? g_app.assoc[g_app.chip_sel]
                                    : (sug_n > 0 ? api_suggest(g_app.chip_sel)
                                                 : g_app.chips[g_app.chip_sel]);

                    snprintf(g_app.query, sizeof(g_app.query), "%s", txt);
                    g_app.query_len = (int)strlen(g_app.query);
                    search_do(g_app.query);
                    search_after_commit();
                } else if (row == -2) {
                    char out[64];

                    if (ime_select(g_app.ime_sel, out, (int)sizeof(out)) > 0) {
                        query_append(out);
                        mlog("ime commit: %s", out);
                        search_after_commit();
                    }
                }
            }
            return;
        }
    }

    /* 键盘：字母先给输入法 */
    if (btn_repeat(pressed, held, dt, PSP_CTRL_LEFT)) {
        g_app.kb_col--;
        if (g_app.kb_col < 0) g_app.kb_col = kb_cols(g_app.kb_row) - 1;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_RIGHT)) {
        g_app.kb_col++;
        if (g_app.kb_col >= kb_cols(g_app.kb_row)) g_app.kb_col = 0;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        int bot_n = ime_active() ? ime_candidate_count() : 0;
        int sug_n = api_suggest_count();
        int top_n = g_app.assoc_n > 0 ? g_app.assoc_n
                  : (sug_n > 0 ? sug_n : (ime_active() ? 0 : g_app.chips_n));
        int target = bot_n > 0 ? -2 : (top_n > 0 ? -1 : (search_hist_count() > 0 ? -3 : 0));

        if (g_app.kb_row <= 0) g_app.kb_row = target;
        else g_app.kb_row--;
        if (g_app.kb_col >= 9) g_app.kb_col = 9;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        g_app.kb_row++;
        if (g_app.kb_row > 2) g_app.kb_row = 2;
        if (g_app.kb_col >= kb_cols(g_app.kb_row)) g_app.kb_col = kb_cols(g_app.kb_row) - 1;
    }

    if (pressed & PSP_CTRL_CIRCLE) {
        int row = g_app.kb_row, col = g_app.kb_col;

        if (row == 0) {
            search_type_char(kb0[col]);
        } else if (row == 1) {
            if (col < 9) {
                search_type_char(kb1[col]);
            } else if (ime_active()) {
                ime_backspace();
            } else {
                query_backspace();
            }
        } else {
            if (col < 7) {
                search_type_char(kb2[col]);
            } else if (col == 7) {
                if (ime_mode() == IME_ASCII) {
                    query_append(" ");
                } else if (ime_active()) {
                    char out[64];

                    if (ime_candidate_count() > 0 &&
                        ime_select(g_app.ime_sel, out, (int)sizeof(out)) > 0)
                        query_append(out);
                    else if (ime_commit_raw(out, (int)sizeof(out)) > 0)
                        query_append(out);
                    mlog("ime commit: %s", out);
                } else {
                    query_append(" ");
                }
            } else {
                if (ime_active()) {
                    char out[64];

                    if (ime_candidate_count() > 0 &&
                        ime_select(g_app.ime_sel, out, (int)sizeof(out)) > 0)
                        query_append(out);
                    else if (ime_commit_raw(out, (int)sizeof(out)) > 0)
                        query_append(out);
                } else {
                    search_do(g_app.query);
                }
            }
        }
    }
}

/* ---- 封面网格 ---- */
static void grid_open(int idx)
{
    const CardList *cl = api_cards();

    if (idx < 0 || idx >= cl->count) return;
    {
        const Card *k = &cl->items[idx];

        if (k->kind == 2) {                 /* 排行榜：走原来的榜单通道 */
            for (int i = 0; i < api_chart_count(); ++i) {
                if (api_chart(i)->id == k->id) {
                    g_app.busy = 1;
                    api_request_chart(i);
                    return;
                }
            }
            api_request_playlist(k->id);    /* 不在首页榜单里就直接当歌单取 */
            return;
        }
        if (k->kind == 3) {                 /* 歌手 → 热门歌曲 */
            api_request_artist_songs(k->id, k->name);
            return;
        }
        api_request_playlist(k->id);
    }
}

static void input_grid(unsigned int pressed, unsigned int held, float dt)
{
    const CardList *cl = api_cards();
    int pages = (cl->count + 9) / 10;

    if (pages < 1) pages = 1;

    {
        int cols = 5;

        if (btn_repeat(pressed, held, dt, PSP_CTRL_LEFT)) {
            if (g_app.grid_sel % cols > 0) g_app.grid_sel--;
        }
        if (btn_repeat(pressed, held, dt, PSP_CTRL_RIGHT)) {
            if (g_app.grid_sel % cols < cols - 1) g_app.grid_sel++;
        }
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        if (g_app.grid_sel >= 5) g_app.grid_sel -= 5;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        if (g_app.grid_sel < 5) g_app.grid_sel += 5;
    }
    if (pressed & PSP_CTRL_LTRIGGER) {
        if (g_app.grid_page > 0) g_app.grid_page--;
    }
    if (pressed & PSP_CTRL_RTRIGGER) {
        if (g_app.grid_page + 1 < pages) g_app.grid_page++;
    }
    if (pressed & PSP_CTRL_CIRCLE) {
        if (cl->count == 0) {
            if (g_app.grid_kind == 3) api_request_artist_top();
            else if (g_app.grid_kind == 1) api_request_my_lists();
            else if(g_app.grid_kind==5)api_request_saved_playlists();
            else api_request_recommend_lists();
            ui_toast("重新加载");
            return;
        }
        grid_open(g_app.grid_page * 10 + g_app.grid_sel);
    }
    if (pressed & PSP_CTRL_SQUARE) {
        /* 播放全部：把整个歌单取回来再从头播 */
        int idx = g_app.grid_page * 10 + g_app.grid_sel;

        if (idx < cl->count && (cl->items[idx].kind == 0 || cl->items[idx].kind == 1)) {
            g_app.play_all_next = 1;
            api_request_playlist(cl->items[idx].id);
        }
    }
    if(pressed&PSP_CTRL_TRIANGLE){int idx=g_app.grid_page*10+g_app.grid_sel;if(idx>=0&&idx<cl->count){toggle_playlist_favorite(&cl->items[idx]);if(g_app.grid_kind==5)api_request_saved_playlists();}}
    if (pressed & PSP_CTRL_CROSS) set_screen(SC_HOME);
}

/* ---- 社区：歌单广场 ---- */
static void input_square(unsigned int pressed, unsigned int held, float dt)
{
    const CardList *cl = api_discovery_cards();
    /* Native fallback has eight category labels; Metro's -1 means recommend. */
    if (g_app.cat_sel < -1 || g_app.cat_sel > 7) g_app.cat_sel = 0;
    int pages = (cl->count + 11) / 12;

    if (pages < 1) pages = 1;

    if (g_app.cat_focus == 0) {
        if (btn_repeat(pressed, held, dt, PSP_CTRL_LEFT)) {
            if (g_app.cat_sel > 0) {
                g_app.cat_sel--;
                g_app.grid_sel = 0;
                g_app.grid_page = 0;
                api_request_discovery(g_app.cat_sel);
            }
        }
        if (btn_repeat(pressed, held, dt, PSP_CTRL_RIGHT)) {
            if (g_app.cat_sel < 7) {
                g_app.cat_sel++;
                g_app.grid_sel = 0;
                g_app.grid_page = 0;
                api_request_discovery(g_app.cat_sel);
            }
        }
        if (pressed & PSP_CTRL_DOWN) {
            g_app.cat_focus = 1;
            g_app.grid_sel = 0;
        }
        if (pressed & PSP_CTRL_CIRCLE) {
            g_app.cat_focus = 1;
            g_app.grid_sel = 0;
        }
        if (pressed & PSP_CTRL_CROSS) set_screen(SC_HOME);
        return;
    }

    if (btn_repeat(pressed, held, dt, PSP_CTRL_LEFT)) {
        if (g_app.grid_sel % 6 > 0) g_app.grid_sel--;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_RIGHT)) {
        if (g_app.grid_sel % 6 < 5) g_app.grid_sel++;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        if (g_app.grid_sel >= 6) g_app.grid_sel -= 6;
        else g_app.cat_focus = 0;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        if (g_app.grid_sel < 6) g_app.grid_sel += 6;
    }
    if (pressed & PSP_CTRL_LTRIGGER) {
        if (g_app.grid_page > 0) g_app.grid_page--;
    }
    if (pressed & PSP_CTRL_RTRIGGER) {
        if (g_app.grid_page + 1 < pages) g_app.grid_page++;
    }
    if (pressed & PSP_CTRL_CIRCLE) {
        int idx = g_app.grid_page * 12 + g_app.grid_sel;

        if (idx < cl->count) {
            g_app.list_from = SC_SQUARE;
            api_request_playlist(cl->items[idx].id);
        }
    }
    if (pressed & PSP_CTRL_SQUARE) {
        if (g_app.grid_page + 1 < pages) g_app.grid_page++;
        else g_app.grid_page = 0;
    }
    if(pressed&PSP_CTRL_TRIANGLE){int idx=g_app.grid_page*12+g_app.grid_sel;if(idx>=0&&idx<cl->count)toggle_playlist_favorite(&cl->items[idx]);}
    if (pressed & PSP_CTRL_CROSS) set_screen(SC_HOME);
}
/* ---- 评论 ---- */
static int comment_input_measure(const char *s, int n) { return gfx_text_w_n(FS_SM, s, n); }
static void input_comments(unsigned int pressed, unsigned int held, float dt)
{
    const CommentList *cm = api_comments();
    CommentView *v = api_comment_view();
    const CommentReplies *r;
    const int viewport = v->viewport > 0 ? v->viewport : 140;
    int width = SCR_W - 70, y, h, total;
    if (v->generation != api_comment_generation() || v->page != api_comment_page()) {
        v->generation = api_comment_generation(); v->page = api_comment_page();
        v->selected = 0; v->expanded = 0; g_app.cm_scroll = 0;
    }
    r = api_comment_replies(v->expanded);
    if (v->selected >= cm->count) v->selected = cm->count ? cm->count - 1 : 0;
    /* A tall selected row scrolls within itself before changing selection. */
    y = 0;
    for (int i = 0; i < v->selected; ++i)
        y += comment_row_height(&cm->items[i], v->expanded, r, width, comment_input_measure);
    h = cm->count ? comment_row_height(&cm->items[v->selected], v->expanded, r, width, comment_input_measure) : 0;
    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        if (g_app.cm_scroll > y) { g_app.cm_scroll -= 24; if (g_app.cm_scroll < y) g_app.cm_scroll = (float)y; }
        else if (v->selected > 0) { --v->selected; g_app.cm_scroll = (float)(y - comment_row_height(&cm->items[v->selected], v->expanded, r, width, comment_input_measure)); }
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        if (g_app.cm_scroll + viewport < y + h) g_app.cm_scroll += 24;
        else if (v->selected + 1 < cm->count) { ++v->selected; g_app.cm_scroll = (float)(y + h); }
    }
    int page_up = btn_repeat(pressed, held, dt, PSP_CTRL_LEFT);
    int page_down = btn_repeat(pressed, held, dt, PSP_CTRL_RIGHT);
    if (page_up) g_app.cm_scroll -= viewport;
    if (page_down) g_app.cm_scroll += viewport;
    total = comment_list_height(cm, v, r, width, comment_input_measure);
    g_app.cm_scroll = comment_scroll_clamp(g_app.cm_scroll, total, viewport);
    if (page_up || page_down)
        v->selected = comment_at_pixel(cm, v, r, width, comment_input_measure, (int)g_app.cm_scroll);
    if ((pressed & PSP_CTRL_CIRCLE) && cm->count) {
        long long cid = cm->items[v->selected].id;
        if (cid && v->expanded == cid) v->expanded = 0;
        else if (cid) { v->expanded = cid; api_request_comment_replies(api_comment_target(), cid); }
    }
    if(pressed&PSP_CTRL_LTRIGGER)g_app.cm_scroll-=viewport;
    if(pressed&PSP_CTRL_RTRIGGER)g_app.cm_scroll+=viewport;
    g_app.cm_scroll=comment_scroll_clamp(g_app.cm_scroll,total,viewport);
    if((pressed&PSP_CTRL_SQUARE)&&api_comment_target()!=api_comment_song())api_request_comments(api_comment_target(),0);
    else if((g_app.cm_scroll+viewport>=total-viewport/2||(pressed&PSP_CTRL_SQUARE))&&api_comments_has_more()&&!api_comments_loading_more())api_request_comments_more();
    if (pressed & PSP_CTRL_TRIANGLE) set_screen(SC_PLAYER);
    if (pressed & PSP_CTRL_CROSS)
        set_screen(g_app.comment_from ? g_app.comment_from : SC_PLAYER);
}

/* ---- 我的 ---- */
static void input_mine(unsigned int pressed, unsigned int held, float dt)
{
    g_app.tab = 1;
    if (!api_user_avatar()[0] && api_logged_in()) api_request_user_detail();
    if (api_logged_in()) api_request_liked_total();

    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        if (g_app.mine_sel > 0) g_app.mine_sel--;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        if (g_app.mine_sel < 11) g_app.mine_sel++;
    }

    if (pressed & PSP_CTRL_LTRIGGER) switch_tab(-1);
    if (pressed & PSP_CTRL_RTRIGGER) switch_tab(1);

    if (pressed & PSP_CTRL_CIRCLE) {
        switch (g_app.mine_sel) {
        case 0: api_request_like_state();api_request_liked(); break;
        case 1: api_request_played(); break;
        case 2:
            g_app.cards_stay = 0;
            g_app.grid_sel = 0;
            g_app.grid_page = 0;
            g_app.grid_kind = 1;
            g_app.cards_open=1;
            api_request_my_lists();
            break;
        case 3:
            g_app.cache_tab = 1;
            g_app.local_sel = 0;
            local_rescan();
            set_screen(SC_CACHE);
            break;
        case 4:
            g_app.dl_sel = 0;
            set_screen(SC_DOWNLOAD);
            break;
        case 5:
            g_app.cache_tab = 0;
            g_app.cache_sel = 0;
            set_screen(SC_CACHE);
            break;
        case 6:
            set_screen(SC_SETTINGS);
            break;
        case 7:
            g_app.login_from = SC_MINE;
            set_screen(SC_LOGIN);
            break;
        case 8:
            g_app.cards_stay = 0;
            g_app.grid_sel = 0;
            g_app.grid_page = 0;
            g_app.grid_kind = 4;
            g_app.cards_open=1;
            g_app.cat_focus = 1;
            api_request_square();
            break;
        case 11:
            g_app.cards_stay=0;g_app.grid_sel=g_app.grid_page=0;g_app.grid_kind=5;g_app.cards_open=1;api_request_saved_playlists();break;
        case 9:
            g_app.cards_stay = 0;
            g_app.grid_sel = 0;
            g_app.grid_page = 0;
            g_app.grid_kind = 3;
            g_app.cards_open=1;
            api_request_artist_top();
            break;
        default:
            set_screen(SC_HELP);
            break;
        }
    }
    if (pressed & PSP_CTRL_TRIANGLE) {
        g_app.login_from = SC_MINE;
        set_screen(SC_LOGIN);
    }
    if (pressed & PSP_CTRL_CROSS) set_screen(SC_HOME);
}
/* ---- 备用音源 ---- */
static void input_alt(unsigned int pressed, unsigned int held, float dt)
{
    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        if (g_app.alt_sel > 0) g_app.alt_sel--;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        if (g_app.alt_sel < ALT_SOURCE_COUNT - 1) g_app.alt_sel++;
    }
    if (pressed & PSP_CTRL_CIRCLE) {
        int i = g_app.alt_sel;
        int on = !alt_enabled(i);

        if (i == ALT_GATEWAY && on && !alt_gateway()[0]) {
            ui_toast("先在 alt.txt 写 gateway_host=IP:端口");
            return;
        }
        alt_set_enabled(i, on);
        alt_save();
        ui_toast(on ? "已开启" : "已关闭");
    }
    if (pressed & PSP_CTRL_CROSS) set_screen(SC_SETTINGS);
}

/* ---- 账号 / 扫码登录 ---- */
static float g_qr_poll_t;

static void input_login(unsigned int pressed, unsigned int held, float dt)
{
    (void)held;

    /* 每 2 秒问一次扫码状态；二维码过期了就自动换一张 */
    g_qr_poll_t += dt;
    if (g_qr_poll_t >= 2.0f) {
        g_qr_poll_t = 0;
        if (api_qr_state() == 801 || api_qr_state() == 802) api_request_qr_poll();
        else if (api_qr_state() == 800 && !api_logged_in()) api_request_qr_new();
    }

    if (pressed & PSP_CTRL_CIRCLE) {
        if (api_logged_in()) {
            ui_toast("已登录，× 返回");
        } else {
            g_qr_poll_t = 0;
            api_request_qr_new();
            ui_toast("正在获取二维码");
        }
    }
    if (pressed & PSP_CTRL_TRIANGLE) {
        if (api_logged_in()) {
            api_logout();
            ui_toast("已退出登录");
        } else {
            api_request_qr_new();
            ui_toast("刷新二维码");
        }
    }
    if (pressed & PSP_CTRL_CROSS)
        set_screen(g_app.login_from ? g_app.login_from : SC_MINE);
}
/* ---- 启动更新弹窗（模态，优先于其它输入） ---- */
static void input_upd_dialog(unsigned int pressed)
{
    int st=api_update_state();
    static char lines[192][112];int count=update_log_lines(api_update_changelog(),lines,192);
    if((pressed&PSP_CTRL_UP)&&g_app.upd_scroll>0)--g_app.upd_scroll;
    if(pressed&PSP_CTRL_DOWN)++g_app.upd_scroll;
    g_app.upd_scroll=update_log_clamp(g_app.upd_scroll,count);
    if(st==4||st==7){
        if(pressed&PSP_CTRL_CROSS){api_update_abort();g_app.upd_dlg=0;g_app.upd_dismissed=1;}
        return;
    }
    if(pressed&PSP_CTRL_CIRCLE){
        if(st==3){api_request_update_download();g_app.upd_scroll=0;}
        else if(st!=1){g_app.upd_dlg=0;g_app.upd_dismissed=1;}
        return;
    }
    if(pressed&PSP_CTRL_CROSS){g_app.upd_dlg=0;g_app.upd_dismissed=1;return;}
}

/* ---- 下载 ---- */
static void download_follow_focus(void){
 static DlItem items[DL_QUEUE_MAX];int count=dl_queue_snapshot(items,DL_QUEUE_MAX);
 if(g_dl_focus_song)for(int i=0;i<count;++i)if(items[i].id==g_dl_focus_song){g_app.dl_sel=i;return;}
 if(g_app.dl_sel>=count)g_app.dl_sel=count?count-1:0;
 g_dl_focus_song=count?items[g_app.dl_sel].id:0;
}

/* Snapshot download songs separately from the mutable browsing list. */
static void select_download_play(const DlItem *items, int count, int index)
{
    if (count <= 0 || index < 0 || index >= count) {
        ui_toast("请先选择下载任务");
        return;
    }
    g_playback.source = PLAYBACK_ONLINE;
    memset(&g_playback.data.online, 0, sizeof(g_playback.data.online));
    g_playback.count = g_playback.data.online.count = count;
    g_playback.index = index;
    for (int i = 0; i < count; ++i)
        g_playback.data.online.songs[i] = items[i].song;
    play_online_context_index(index);
}

static void input_download(unsigned int pressed, unsigned int held, float dt)
{
    if (pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        g_app.dl_tab ^= 1;
        g_app.dl_sel = 0;
        g_app.list_scroll = 0;
        ui_toast(g_app.dl_tab ? "MV 下载" : "歌曲下载");
        return;
    }

    if (g_app.dl_tab) {
        MvDlItem items[MV_DL_MAX];
        int n = mv_dl_snapshot(items, MV_DL_MAX);

        if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
            if (g_app.dl_sel > 0) g_app.dl_sel--;
        }
        if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
            if (g_app.dl_sel < n - 1) g_app.dl_sel++;
        }
        if (pressed & PSP_CTRL_CIRCLE) {
            if (n > 0) {
                if (items[g_app.dl_sel].state == 1) {
                    api_mv_download_abort();
                    ui_toast("已取消");
                } else if(items[g_app.dl_sel].state==3){
                    int queued=api_request_mv_save(items[g_app.dl_sel].mvid);ui_toast(queued<0?"任务忙，请稍后重试":"继续下载，正在校验已有文件");
                } else {
                    g_mv_restore_pause = audio_paused();
                    g_mv_from = SC_DOWNLOAD;
                    audio_pause(1);
                    if(mvx_active()){ui_toast("MV导出中，请稍后播放");return;}
                    if (mv_start(items[g_app.dl_sel].mvid, items[g_app.dl_sel].path,
                                 items[g_app.dl_sel].name) == 0) {
                        g_app.dl_tab = 0;
                        g_app.dl_sel = 0;
                        set_screen(SC_MV);
                    } else {
                        if(!mv_active()){skin_runtime_resume();ui_theme_changed();}
    audio_pause(g_mv_restore_pause);
                        ui_toast(mv_block_reason() == 1 ? "内存不足：请重启后再看 MV"
                                                        : mv_block_reason()==2?"解码器初始化失败，请先尝试软件解码":"MV 启动失败");
                    }
                }
            }
        }
        if((pressed&PSP_CTRL_SQUARE)&&n>0){MvDlItem*it=&items[g_app.dl_sel];ui_toast(it->state!=2?"请先等待MV下载完成":mvx_start(it->mvid,it->name,it->artist)==0?"正在导出MV到 VIDEO":"导出未启动，可能正在导出或为本地文件");}
        if (pressed & PSP_CTRL_TRIANGLE) {
            if (n > 0) {
                int deleted=mv_dl_delete(g_app.dl_sel);
                ui_toast(deleted==-2?"本地原文件请用文件管理器删除":deleted<0?"删除失败":"已删除");
                if (!deleted&&g_app.dl_sel >= n - 1 && g_app.dl_sel > 0) g_app.dl_sel--;
            }
        }
        if (pressed & PSP_CTRL_CROSS) set_screen(SC_MINE);
        if (pressed & PSP_CTRL_SELECT) {
            g_app.cache_tab = 0;
            g_app.cache_sel = 0;
            set_screen(SC_CACHE);
        }
        return;
    }

    {
        DlItem items[DL_QUEUE_MAX];
        int n = dl_queue_snapshot(items,DL_QUEUE_MAX);

        if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
            if (g_app.dl_sel > 0) g_app.dl_sel--;
        }
        if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
            if (g_app.dl_sel < n - 1) g_app.dl_sel++;
        }

        if(n>0&&g_app.dl_sel>=0&&g_app.dl_sel<n)g_dl_focus_song=items[g_app.dl_sel].id;
        if (pressed & PSP_CTRL_CIRCLE) {
            if (n > 0) {
                int paused=items[g_app.dl_sel].state==DL_PAUSED;
                int changed=paused?dl_queue_resume(g_app.dl_sel):dl_queue_pause(g_app.dl_sel);
                ui_toast(changed<0?"任务状态已变化，请重试":paused?"继续下载，正在校验断点":"已请求暂停，保留已下载内容");
            }
        }
        if((pressed&PSP_CTRL_TRIANGLE)&&n>0){dl_queue_cancel(g_app.dl_sel);ui_toast("已移除任务，断点文件保留");if(g_app.dl_sel>=dl_queue_count()&&g_app.dl_sel>0)g_app.dl_sel--;}
        if (pressed & PSP_CTRL_SQUARE) {
            if (n > 0) {
                g_app.confirm_clear = 2;
                ui_toast("□ 清空队列：○ 确认  × 取消");
            }
        }
        if (pressed & PSP_CTRL_CROSS) set_screen(SC_MINE);
        if (pressed & PSP_CTRL_SELECT)
            select_download_play(items, n, g_app.dl_sel);
    }
}

/* ---- 缓存 ---- */
static void input_cache(unsigned int pressed, unsigned int held, float dt)
{
    int n=cache_count();
    int ln = local_count();

    /* L/R 切换「缓存 / 本地」 */
    if ((pressed & PSP_CTRL_LTRIGGER) || (pressed & PSP_CTRL_RTRIGGER)) {
        g_app.cache_tab = !g_app.cache_tab;
        ui_toast(g_app.cache_tab ? "本地音乐" : "下载缓存");
    }

    if (g_app.cache_tab == 0) {
        if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
            if (g_app.cache_sel > 0) g_app.cache_sel--;
        }
        if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
            if (g_app.cache_sel < n - 1) g_app.cache_sel++;
        }

        if(btn_repeat(pressed,held,dt,PSP_CTRL_LEFT)){g_app.cache_sel-=8;if(g_app.cache_sel<0)g_app.cache_sel=0;}
        if(btn_repeat(pressed,held,dt,PSP_CTRL_RIGHT)){g_app.cache_sel+=8;if(g_app.cache_sel>=n)g_app.cache_sel=n>0?n-1:0;}
        if (pressed & PSP_CTRL_CIRCLE) {
            if (n > 0) play_cache_entry(g_app.cache_sel);
        }
        CacheEntry selected={0};if(pressed&(PSP_CTRL_TRIANGLE|PSP_CTRL_SQUARE)){const CacheEntry*entry=cache_get(g_app.cache_sel);if(entry)selected=*entry;}
        if (pressed & PSP_CTRL_TRIANGLE) {
            /* 导出到 ms0:/MUSIC，出现在 PSP 自己的音乐里 */
            if (n > 0) {
                Song s;

                memset(&s, 0, sizeof(s));
                s.id = selected.id;
                s.dur_ms = selected.dur_ms;
                snprintf(s.name, sizeof(s.name), "%s", selected.name);
                snprintf(s.artist, sizeof(s.artist), "%s", selected.artist);
                snprintf(s.album, sizeof(s.album), "%s", selected.album);
                if (local_export_active()) {
                    ui_toast("上一首还在导出，稍等");
                } else if (local_export_exists(s.id)) {
                    g_export_song = s;
                    g_app.confirm_clear = 4;
                } else {
                    api_request_export(&s);
                    ui_toast("正在导出到 音乐/MELODY");
                }
            }
        }
        if (pressed & PSP_CTRL_SQUARE) {
            if (n > 0) {
                if (g_app.has_now && selected.id == g_app.now.id) {
                    ui_toast("正在播放这首歌，先切歌再删");
                } else {
                    cache_remove(selected.id);
                    ui_toast("已删除");
                    if (g_app.cache_sel >= cache_count() && g_app.cache_sel > 0)
                        g_app.cache_sel--;
                }
            }
        }
        if (pressed & PSP_CTRL_SELECT) {
            g_app.confirm_clear = 1;
            ui_toast("SELECT 清空缓存：○ 确认  × 取消");
        }
    } else {
        if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
            if (g_app.local_sel > 0) g_app.local_sel--;
        }
        if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
            if (g_app.local_sel < ln - 1) g_app.local_sel++;
        }
        if(btn_repeat(pressed,held,dt,PSP_CTRL_LEFT)){g_app.local_sel-=8;if(g_app.local_sel<0)g_app.local_sel=0;}
        if(btn_repeat(pressed,held,dt,PSP_CTRL_RIGHT)){g_app.local_sel+=8;if(g_app.local_sel>=ln)g_app.local_sel=ln>0?ln-1:0;}
        if (pressed & PSP_CTRL_CIRCLE) {
            if (ln > 0) play_local_file(g_app.local_sel);
        }
        if (pressed & PSP_CTRL_SELECT) {
            local_rescan();
            ui_toast("已重新扫描");
        }
    }

    if (pressed & PSP_CTRL_CROSS) set_screen(SC_MINE);
}

/* ---- 设置 ---- */
static void cycle_quality(int dir)
{
    g_app.quality = (g_app.quality + dir + 3) % 3;
    store_set_quality(g_app.quality);
    ui_toast(g_app.quality == 0 ? "音质：标准 128k"
             : (g_app.quality == 1 ? "音质：较高 192k" : "音质：极高 320k"));
}

static void cycle_parts(int dir)
{
    int n = g_app.dl_parts + dir;

    if (n < 2) n = 2;
    if (n > 6) n = 6;
    g_app.dl_parts = n;
    store_set_dl_parts(g_app.dl_parts);
    dl_set_parts(g_app.dl_parts);
    {
        char b[32];

        snprintf(b, sizeof(b), "下载并发：%d 路", g_app.dl_parts);
        ui_toast(b);
    }
}

static void toggle_theme(void)
{
    g_app.theme = g_app.theme ? 0 : 1;
    store_set_theme(g_app.theme);
    ui_theme_changed();
    ui_toast(g_app.theme ? "深色主题" : "浅色主题（网页版）");
}

static int video_setting_change(int row) {
    int r=0;
    if(row==12){int mode=(mv_decoder_mode()+1)%3;r=mv_set_decoder_mode(mode);if(!r)ui_toast(mode==2?"自动：解析片源选择解码，手动设置可覆盖":mode==1?"硬解：仅兼容AVC，下次播放生效":"软件解码");}
    else if(row==13){int q=(mv_soft_quality()+1)%4;r=mv_set_soft_quality(q);if(!r){const char*names[]={"原画：精确解码，按原始尺寸显示","流畅：优先帧率，画质降低","均衡：精确插值，减少非参考帧滤波","清晰满屏：精确解码与高质量缩放，较慢"};ui_toast(names[q]);}}
    else if(row==14){r=mv_set_stream_enabled(!mv_stream_enabled());if(!r)ui_toast(mv_stream_enabled()?"视频：边下边播，软解和硬解均可用":"视频：完整下载后播放");}
    else if(row==16){int v=mv_resolution();v=v==0?240:v==240?360:v==360?480:0;r=mv_set_resolution(v);if(!r)ui_toast("MV清晰度：下次下载生效，已有缓存保持原片");}
    else if(row==17){int v=mv_buffer_seconds();v=v==3?8:v==8?15:v==15?30:3;r=mv_set_buffer_seconds(v);if(!r)ui_toast("提前缓冲已设置，缓冲不足会暂停等数据");}
    else if(row==15){ui_toast("软解 H.264 / MPEG-4 / MJPEG / FLV1；容器 MP4/FLV/AVI/MKV");}
    else return 0;
    if(r<0)ui_toast("设置保存失败，请检查记忆棒");
    return 1;
}
static void input_skins(unsigned pressed,unsigned held,float dt){
 if(pressed&PSP_CTRL_SELECT){skin_options_dialog_open();return;}
 if(pressed&PSP_CTRL_LTRIGGER){char msg[128];int loaded=skin_sound_loaded_count(),started=skin_sound_play("confirm");snprintf(msg,sizeof(msg),started?"音效试听已送出（%d个已加载）；请检查PSP音量":"音效未播放（%d个已加载）：%s",loaded,skin_sound_error());ui_toast(msg);mlog("skin sound audition loaded=%d started=%d accepted=%u mixed=%u peak=%d",loaded,started,skin_sound_play_count(),skin_sound_mixed_frames(),skin_sound_peak());return;}
 if(pressed&PSP_CTRL_CROSS){if(xmb_active())xmb_go_back();else set_screen(SC_SETTINGS);return;}
 if(api_skin_busy())return;
 int count=skin_catalog_count();
 if(btn_repeat(pressed,held,dt,PSP_CTRL_UP)&&g_app.skin_sel>0)--g_app.skin_sel;
 if(btn_repeat(pressed,held,dt,PSP_CTRL_DOWN)&&g_app.skin_sel<count)++g_app.skin_sel;
 if(pressed&PSP_CTRL_TRIANGLE){api_request_skin(0,NULL);return;}
 if((pressed&PSP_CTRL_CIRCLE)&&!g_app.skin_sel){mlog("skin apply: reset to system default");skin_runtime_reset();ui_theme_changed();ui_toast("已恢复系统默认界面");return;}
 if(g_app.skin_sel>0&&g_app.skin_sel<=count){const SkinCatalogEntry*entry=&skin_catalog_items()[g_app.skin_sel-1];
  if(pressed&PSP_CTRL_SQUARE){api_request_skin(2,entry->id);return;}
  if(pressed&PSP_CTRL_RTRIGGER){api_request_skin(1,entry->id);ui_toast("正在重新下载皮肤最新文件");return;}
  if(pressed&PSP_CTRL_CIRCLE){char root[256],err[192]={0};if(!entry->installed){mlog("skin apply: install requested id=%s",entry->id);api_request_skin(1,entry->id);ui_toast("正在下载并校验皮肤");}
   else if(skin_catalog_root(entry->id,root,sizeof(root))==0&&skin_runtime_apply(root,err,sizeof(err))==0){mlog("skin apply: ok id=%s",entry->id);ui_theme_changed();ui_toast("皮肤已即时应用，无需重启");}
   else {mlog("skin apply: failed id=%s: %s",entry->id,err[0]?err:"catalog root rejected");ui_toast(err[0]?err:"皮肤应用失败，保留原界面");}}
 }
}
static void open_skin_center(void){g_xmb_skin_parent=g_app.screen==SC_SETTINGS?SC_SETTINGS:SC_HOME;g_app.skin_sel=0;g_app.skin_scroll=0;if(!api_skin_busy())skin_catalog_load_local();set_screen(SC_SKINS);}

static void input_settings(unsigned int pressed, unsigned int held, float dt)
{
    int setting=melody_setting_id(g_app.settings_sel);
    if(setting==24&&(pressed&(PSP_CTRL_LEFT|PSP_CTRL_RIGHT|PSP_CTRL_CIRCLE))){
        int on=!store_scan_local_mv();
        store_set_scan_local_mv(on);
        mv_local_video_rescan();
        if(on)ui_toast("已开启：将扫描 VIDEO 文件夹；不推荐，列表会变慢且可能列出播不了的片");
        else ui_toast("已关闭：MV 列表只显示本软件下载的视频");
        return;
    }
    if(setting==23&&(pressed&(PSP_CTRL_LEFT|PSP_CTRL_RIGHT|PSP_CTRL_CIRCLE))){
        int on=!store_disc_player();
        store_set_disc_player(on);
        if(!on)skin_disc_release();
        ui_toast(on?"唱片播放器：开":"唱片播放器：关");
        return;
    }
    if(setting==22&&(pressed&(PSP_CTRL_LEFT|PSP_CTRL_RIGHT|PSP_CTRL_CIRCLE))){
        int target=!store_renderer();
        /* gfx queues the switch for a safe frame boundary. */
        if(gfx_set_renderer(target)==0){store_set_renderer(target);ui_toast(target?"界面渲染：GE / GU":"界面渲染：CPU");}
        else {const char *err=gfx_renderer_error();ui_toast(err&&err[0]?err:"界面渲染切换失败，保留当前设置");}
        return;
    }
    if(setting==3&&(pressed&(PSP_CTRL_CIRCLE|PSP_CTRL_LEFT|PSP_CTRL_RIGHT))){open_skin_center();return;}
    if(setting==21&&(pressed&(PSP_CTRL_LEFT|PSP_CTRL_RIGHT|PSP_CTRL_CIRCLE))){int n=store_prefetch_count();n=(n+((pressed&PSP_CTRL_LEFT)?5:1))%6;store_set_prefetch_count(n);if(!n)api_cancel_pending_prefetch();ui_toast(n?"后续预下载已调整，随机模式与下一首同步":"后续预下载已关闭");return;}
    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        if (g_app.settings_sel > 0) g_app.settings_sel--;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        if (g_app.settings_sel < MELODY_SETTINGS_COUNT-1) g_app.settings_sel++;
    }

    if (pressed & PSP_CTRL_LEFT) {
        if(video_setting_change(setting))return;
        switch (setting) {
        case 0: cycle_quality(-1); break;
        case 1: adjust_volume(-10); break;
        case 2: cycle_parts(-1); break;
        case 3: toggle_theme(); break;
        case 5:
            g_app.show_mem = !g_app.show_mem;
            store_set_show_mem(g_app.show_mem);
            ui_toast(g_app.show_mem ? "内存显示：开" : "内存显示：关");
            break;
        default: break;
        }
    }
    if (pressed & PSP_CTRL_RIGHT) {
        if(video_setting_change(setting))return;
        switch (setting) {
        case 0: cycle_quality(1); break;
        case 1: adjust_volume(10); break;
        case 2: cycle_parts(1); break;
        case 3: toggle_theme(); break;
        case 5:
            g_app.show_mem = !g_app.show_mem;
            store_set_show_mem(g_app.show_mem);
            ui_toast(g_app.show_mem ? "内存显示：开" : "内存显示：关");
            break;
        default: break;
        }
    }

    if (pressed & PSP_CTRL_TRIANGLE) {
        g_app.login_from = SC_SETTINGS;
        set_screen(SC_LOGIN);
        g_qr_poll_t = 0;
        if (!api_logged_in() && api_qr_state() == 0) api_request_qr_new();
        else if (api_logged_in() && !api_login_name()[0]) api_request_account();
        return;
    }

    if (pressed & PSP_CTRL_CIRCLE) {
        if(video_setting_change(setting))return;
        switch (setting) {
        case 0: cycle_quality(1); break;
        case 1: adjust_volume(10); break;
        case 2: cycle_parts(1); break;
        case 3: toggle_theme(); break;
        case 4: open_wifi_screen(); break;
        case 9: {
            int st=api_update_state();g_app.upd_dlg=1;g_app.upd_dismissed=0;g_app.upd_scroll=0;g_app.upd_sel=1;
            if(st!=3&&st!=4&&st!=7&&st!=5)api_request_update_check();
            break;
        }
        case 10:
            g_app.help_page = 0;
            set_screen(SC_HELP);
            break;
        case 18:
            notice_seen=0;if(!community_announcement_get()){ui_toast("正在检查存储桶公告");api_request_announcement_check();}break;
        case 19:
            policy_open=1;policy_scroll=0;break;
        case 20:
            store_set_show_speed(!store_show_speed());ui_toast(store_show_speed()?"网速显示：开":"网速显示：关");break;
        case 11:
            g_app.alt_sel = 0;
            set_screen(SC_ALT);
            break;
        case 5:
            g_app.show_mem = !g_app.show_mem;
            store_set_show_mem(g_app.show_mem);
            ui_toast(g_app.show_mem ? "内存显示：开" : "内存显示：关");
            break;
        case 6:
            g_app.confirm_clear = 2;
            break;
        case 7:
            g_app.confirm_clear = 1;
            break;
        case 8:
            g_app.login_from = SC_SETTINGS;
            set_screen(SC_LOGIN);
            g_qr_poll_t = 0;
            if (!api_logged_in() && api_qr_state() == 0) api_request_qr_new();
            break;
        default:
            g_app.help_page = 0;
            set_screen(SC_HELP);
            break;
        }
    }

    if (pressed & PSP_CTRL_CROSS) set_screen(SC_MINE);
    if (pressed & PSP_CTRL_SELECT) open_wifi_screen();
}

/* ---- 网络 ---- */
static void input_boot(unsigned int pressed)
{
    if (pressed & PSP_CTRL_CIRCLE) open_wifi_screen();
    if (pressed & PSP_CTRL_SELECT) {
        net_utility_dialog();
        refresh_profiles();
    }
}

static void input_wifi(unsigned int pressed, unsigned int held, float dt)
{
    if (btn_repeat(pressed, held, dt, PSP_CTRL_UP)) {
        if (g_app.profile_sel > 0) g_app.profile_sel--;
    }
    if (btn_repeat(pressed, held, dt, PSP_CTRL_DOWN)) {
        if (g_app.profile_sel < g_app.profile_count - 1) g_app.profile_sel++;
    }

    if (pressed & PSP_CTRL_CIRCLE) {
        if (net_wlan_switch() == 0) {
            g_app.wlan_on = 0;
            g_app.connecting = 3;
            snprintf(g_app.connect_msg, sizeof(g_app.connect_msg),
                     "WLAN 开关是关的：把机身侧面的拨钮拨到 ON");
            return;
        }
        if (g_app.profile_count > 0) {
            g_app.connecting = 1;
            snprintf(g_app.connect_msg, sizeof(g_app.connect_msg), "%s",
                     g_app.profiles[g_app.profile_sel].name);
            api_request_connect(g_app.profiles[g_app.profile_sel].id);
        } else {
            g_app.connecting = 3;
            snprintf(g_app.connect_msg, sizeof(g_app.connect_msg),
                     "还没有连接：按 △ 打开系统网络设置新建一个");
        }
    }

    if (pressed & PSP_CTRL_SELECT) {
        refresh_profiles();
        ui_toast("已刷新");
    }

    if (pressed & PSP_CTRL_TRIANGLE) {
        net_utility_dialog();
        refresh_profiles();
        api_request_probe();
    }

    if (pressed & PSP_CTRL_CROSS) {
        if (g_app.has_now) set_screen(SC_PLAYER);
        else set_screen(SC_HOME);
    }
}

/* ------------------------------------------------------------------ */
/* 自动化测试（dev.flag）                                              */
/* ------------------------------------------------------------------ */

static void shot_at(float sec)
{
    g_shot_t = g_app.time + sec;
    g_shot_done = 0;
}

/* 巡检用假数据：不联网也能把所有界面画出来 */
static void fake_song_list(int n, int kind, const char *title)
{
    static const char *nm[8] = { "海屿你", "晴天", "夜曲", "稻香", "演员",
                                 "泡沫", "成都", "起风了" };
    static const char *ar[8] = { "马也_Crabbit", "周杰伦", "周杰伦", "周杰伦", "薛之谦",
                                 "邓紫棋", "赵雷", "买辣椒也用券" };
    static const char *al[4] = { "海屿你", "叶惠美", "十一月的萧邦", "无法长大" };

    g_list.count = n;
    snprintf(g_list.title, sizeof(g_list.title), "%s", title);
    g_list.kind = kind;
    for (int i = 0; i < n; ++i) {
        Song *s = &g_list.songs[i];

        memset(s, 0, sizeof(*s));
        s->id = 900000000LL + i;
        snprintf(s->name, sizeof(s->name), "%s%s", nm[i % 8], i >= 8 ? " (Live)" : "");
        snprintf(s->artist, sizeof(s->artist), "%s", ar[i % 8]);
        snprintf(s->album, sizeof(s->album), "%s", al[i % 4]);
        s->dur_ms = 180000 + (i % 7) * 23000;
        s->fee = (i % 9 == 3) ? 1 : 0;
        s->hot = (i < 3);
        s->pop = 100 - i;
        s->playable = (i % 11 == 5) ? 2 : 1;
    }
}

static void auto_test(float dt)
{
    int mode = dev_mode();
    float t = g_app.time;

    if (mode < 3) return;

    g_test_t += dt;

    if (!g_shot_done && g_shot_t > 0 && t >= g_shot_t) {
        shot("auto");
        g_shot_done = 1;
    }

    if (mode == 31) {
        /* 点赞任务化巡检：用不存在的 id，避免动到真实账号的喜欢列表 */
        static int s31;

        if (s31 == 0 && g_test_t > 3.0f) {
            s31 = 1;
            g_test_t = 0;
            mlog("TEST31 like begin frame=%d", g_frame);
            api_request_like(999999999999LL, 1);
        } else if (s31 == 1 && g_test_t > 4.0f) {
            s31 = 2;
            mlog("TEST31 like returned frame=%d (UI kept running)", g_frame);
        }
        return;
    }

    if (mode == 30) {
        /* UI 巡检 v2：把 16 个界面（含滚动状态）全部走一遍，不依赖网络 */
        static int s30;
        static const char *tags[26] = {
            "home_hero", "home_chart", "home_cover", "home_cover2", "home_quick",
            "list", "list_hero", "player", "lyrics", "search_kb",
            "search_result", "grid", "square", "comments", "mine",
            "settings", "help", "login", "download", "cache",
            "cache_local", "wifi", "player_menu", "confirm", "home_dark",
            "mv"
        };

        if (s30 == 0) {
            /* 进来先准备假数据 */
            fake_song_list(30, LK_CHART, "热歌榜");
            snprintf(g_list_title, sizeof(g_list_title), "%s", g_list.title);
            g_list_kind = g_list.kind;
            s30 = 1;
        }

        if (g_test_t > 2.2f) {
            int step = g_test_step;

            g_test_t = 0;
            if (step >= 25) return;
            g_test_step = step + 1;
            g_app.tab = 0;
            g_app.enter = 0;
            g_app.confirm_clear = 0;
            g_app.menu_open = 0;

            switch (step) {
            case 0: g_app.home_sel = HOME_ITEM_HERO; g_app.home_scroll = 0; set_screen(SC_HOME); break;
            case 1: g_app.home_sel = HOME_ITEM_CHART + 1; set_screen(SC_HOME); break;
            case 2: g_app.home_sel = HOME_ITEM_COVER + 2; set_screen(SC_HOME); break;
            case 3: g_app.home_sel = HOME_ITEM_COVER + 7; set_screen(SC_HOME); break;
            case 4: g_app.home_sel = HOME_ITEM_QUICK + 3; set_screen(SC_HOME); break;
            case 5:
                fake_song_list(20, LK_CHART, "热歌榜");
                snprintf(g_list_title, sizeof(g_list_title), "%s", g_list.title);
                g_list_kind = LK_CHART;
                g_app.list_sel = 3;
                g_app.list_scroll = 0;
                set_screen(SC_LIST);
                break;
            case 6:
                fake_song_list(40, LK_PLAYLIST, "我喜欢的音乐 · 测试歌单");
                snprintf(g_list_title, sizeof(g_list_title), "%s", g_list.title);
                g_list_kind = LK_PLAYLIST;
                g_app.list_sel = 0;
                g_app.list_scroll = 0;
                set_screen(SC_LIST);
                break;
            case 7:
            case 8:
                g_app.now = g_list.songs[0];
    g_app.is_liked = 0;
                g_app.has_now = 1;
                g_app.paused = 0;
                g_app.playing_index = 0;
                set_screen(step == 7 ? SC_PLAYER : SC_LYRICS);
                break;
            case 9:
                g_app.tab = 2;
                g_app.kb_edit = 1;
                g_app.kb_row = 0;
                g_app.kb_col = 3;
                g_app.search_state = 0;
                snprintf(g_app.query, sizeof(g_app.query), "zhoujielun");
                g_app.query_len = (int)strlen(g_app.query);
                set_screen(SC_SEARCH);
                break;
            case 10:
                g_app.tab = 2;
                g_app.kb_edit = 0;
                g_app.search_state = 2;
                g_app.list_sel = 2;
                g_app.list_scroll = 0;
                g_list.kind = LK_SEARCH;
                snprintf(g_list.title, sizeof(g_list.title), "搜索");
                g_list_kind = LK_SEARCH;
                set_screen(SC_SEARCH);
                break;
            case 11:
                g_app.grid_kind = 0;
                g_app.grid_page = 0;
                g_app.grid_sel = 2;
                set_screen(SC_GRID);
                break;
            case 12:
                g_app.grid_kind = 4;
                g_app.grid_page = 0;
                g_app.grid_sel = 3;
                g_app.cat_focus = 1;
                g_app.cat_sel = 0;
                set_screen(SC_SQUARE);
                break;
            case 13:
                g_app.cm_scroll = 0;
                g_app.comment_from = SC_MINE;
                set_screen(SC_COMMENTS);
                break;
            case 14:
                g_app.tab = 1;
                g_app.mine_sel = 0;
                set_screen(SC_MINE);
                break;
            case 15:
                g_app.settings_sel = 0;
                set_screen(SC_SETTINGS);
                break;
            case 16: set_screen(SC_HELP); break;
            case 17: set_screen(SC_LOGIN); break;
            case 18: set_screen(SC_DOWNLOAD); break;
            case 19: g_app.cache_tab = 0; set_screen(SC_CACHE); break;
            case 20: g_app.cache_tab = 1; set_screen(SC_CACHE); break;
            case 21: set_screen(SC_WIFI); break;
            case 22:
                g_app.now = g_list.songs[0];
    g_app.is_liked = 0;
                g_app.has_now = 1;
                g_app.playing_index = 0;
                g_app.menu_open = 1;
                g_app.menu_sel = 2;
                set_screen(SC_PLAYER);
                break;
            case 23:
                g_app.cache_tab = 0;
                g_app.confirm_clear = 1;
                set_screen(SC_CACHE);
                break;
            case 24:
                g_app.theme = 1;
                ui_theme_changed();
                g_app.home_sel = HOME_ITEM_CHART;
                set_screen(SC_HOME);
                break;
            default: break;
            }
            mlog("TEST30 step %d %s", step, tags[step]);
            shot_at(1.5f);
        }
        return;
    }

    if (mode == 3) {
        /* 界面巡回：每个屏幕停 1.6 秒，切过去 1.3 秒后截图 */
        static const int order[] = { SC_HOME, SC_LIST, SC_PLAYER, SC_LYRICS,
                                     SC_SEARCH, SC_DOWNLOAD, SC_CACHE, SC_SETTINGS, SC_HELP };
        int step = (int)(t / 1.9f);

        if (step != g_test_step && step < (int)(sizeof(order) / sizeof(order[0]))) {
            g_test_step = step;
            if (step == 0) {
                g_app.busy = 1;
                api_request_chart(0);
            } else if (step == 2) {
                if (g_list.count > 0) {
                    for (int i = 0; i < g_list.count; ++i)
                        if (g_list.songs[i].playable != 2) { play_index(i); break; }
                }
            }
            if (step == 4) {
                g_app.kb_edit = 0;
                g_app.search_state = 2;
            }
            set_screen(order[step]);
            g_shot_t = t + 1.2f;
            g_shot_done = 0;
        }
        return;
    }

    if (mode == 4) {
        /* 快进测试：开播后每 3.5 秒快进 15 秒 */
        static int seeks;
        if (g_app.screen == SC_PLAYER && audio_state() == AUDIO_PLAY && g_test_t > 3.5f) {
            g_test_t = 0;
            if (seeks < 8) {
                seeks++;
                mlog("TEST seek #%d from %dms state=%d", seeks, audio_pos_ms(), audio_state());
                audio_seek(audio_pos_ms() + 15000);
            } else if (seeks == 8) {
                seeks++;
                mlog("TEST seek done, pos=%dms state=%d err=%s", audio_pos_ms(),
                     audio_state(), audio_error());
            }
        }
        return;
    }

    if (mode == 6) {
        /* 下载测试：榜单前三首加入下载，然后盯着下载页 */
        if (g_test_step <= 0 && g_list.count >= 3) {
            int n = 0;

            g_test_step = 1;
            for (int i = 0; i < g_list.count && n < 3; ++i) {
                if (g_list.songs[i].playable == 2) continue;
                api_request_download(&g_list.songs[i]);
                n++;
            }
            set_screen(SC_DOWNLOAD);
            shot_at(2.5f);
            mlog("TEST download enqueued %d", n);
        } else if (g_test_step == 1 && g_shot_done && dl_queue_count() > 0 &&
                   g_app.time > g_shot_t + 3.0f) {
            g_test_step = 2;
            shot_at(0.2f);
            mlog("TEST download shot 2");
        }
        return;
    }

    if (mode == 7) {
        /* 搜索测试：先看键盘，再真的搜一次 */
        if (g_test_step <= 0 && g_test_t > 1.5f) {
            g_test_step = 1;
            g_app.kb_edit = 1;
            g_app.kb_row = 0;
            g_app.kb_col = 0;
            g_app.query[0] = 0;
            g_app.query_len = 0;
            set_screen(SC_SEARCH);
            shot_at(1.2f);
        } else if (g_test_step == 1 && g_shot_done) {
            g_test_step = 2;
            snprintf(g_app.query, sizeof(g_app.query), "zhoujielun");
            g_app.query_len = (int)strlen(g_app.query);
            search_do(g_app.query);
            shot_at(3.5f);
            mlog("TEST search go");
        }
        return;
    }

    if (mode == 8) {
        /* 缓存完成后再快进：验证「下载完成后还能继续播/seek」 */
        static int phase, seeks;

        if (phase == 0) {
            if (g_app.screen == SC_PLAYER && audio_state() == AUDIO_PLAY) {
                phase = 1;
                g_test_t = 0;
                mlog("TEST wait for cache complete");
            }
        } else if (phase == 1) {
            if (audio_cache_pct() < 0) {
                phase = 2;
                g_test_t = 0;
                mlog("TEST cache complete at %dms, state=%d", audio_pos_ms(), audio_state());
            } else if (g_test_t > 120.0f) {
                phase = 3;
                mlog("TEST timeout waiting cache");
            }
        } else if (phase == 2 && g_test_t > 2.5f) {
            g_test_t = 0;
            seeks++;
            if (seeks <= 6) {
                mlog("TEST post-cache seek #%d from %dms state=%d", seeks, audio_pos_ms(),
                     audio_state());
                audio_seek(audio_pos_ms() + 12000);
            } else {
                mlog("TEST post-cache done pos=%d state=%d err=%s", audio_pos_ms(),
                     audio_state(), audio_error());
                phase = 3;
            }
        }
        return;
    }

    if (mode == 14) {
        /* 音频 dump：播一首缓存歌，把输出 PCM 存到记忆棒供离线分析 */
        static int step;

        if (step == 0 && cache_count() > 0) {
            step = 1;
            play_cache_entry(0);
        } else if (step == 1 && audio_state() == AUDIO_PLAY) {
            step = 2;
            g_test_t = 0;
            mlog("TEST14 dump start");
            audio_debug_dump(400);      /* 400 块 ≈ 10 秒 */
        }
        return;
    }

    if (mode == 24) {
        /* 直接播《人是猫》3395708493（我喜欢的音乐第一首） */
        static int s24;

        if (s24 == 0 && g_test_t > 1.0f) {
            s24 = 1; g_test_t = 0;
            g_list.count = 1;
            g_list.songs[0].id = 3395708493LL;
            snprintf(g_list.songs[0].name, sizeof(g_list.songs[0].name), "人是猫");
            snprintf(g_list.songs[0].artist, sizeof(g_list.songs[0].artist), "张卡斯");
            g_list.songs[0].dur_ms = 200000;
            g_app.playing_index = 0;
            api_request_play(0, &g_list.songs[0]);
        } else if (s24 == 1 && g_test_t > 12.0f) {
            s24 = 2; g_test_t = 0;
            mlog("TEST24 st=%d pos=%d err=%s", audio_state(), audio_pos_ms(), audio_error());
            shot("renshimao");
        }
        return;
    }

    if (mode == 61) {
        /* MV 边下边播：请求下载后由主循环在下到阈值时自动起播 */
        static int s61;

        if (s61 == 0 && g_test_t > 8.0f) {
            s61 = 1; g_test_t = 0;
            g_app.has_now = 1;
            g_app.now.mv = 34729519;
            snprintf(g_app.now.name, sizeof(g_app.now.name), "Stream Test");
            set_screen(SC_PLAYER);
            mlog("TEST61 request mv stream");
            api_request_mv_download(34729519);
            g_mv_pending = 34729519;
        } else if (s61 == 1 && g_test_t > 120.0f) {
            s61 = 2;
            mlog("TEST61 done screen=%d avail=%lld total=%lld", g_app.screen,
                 mv_stream_available(), mv_stream_total());
        }
        return;
    }

    if(mode==67){static int asked,last=-1;if(!asked&&g_test_t>1.5f){asked=1;char page[16]={0};FILE*f=fopen(APP_DIR "updatetestpage.txt","r");if(f){fgets(page,sizeof(page),f);fclose(f);}set_screen(!strncmp(page,"settings",8)?SC_SETTINGS:SC_HOME);api_request_update_check();}int st=api_update_state();if(st!=last){last=st;mlog("TEST67 update state=%d progress=%d",st,api_update_progress());log_flush();}if(st==3&&!g_app.upd_dismissed)g_app.upd_dlg=1;return;}
    if(mode==66){static int stage;if(stage==0&&g_test_t>1.5f){set_screen(SC_HELP);g_app.help_page=5;stage=1;g_test_t=0;}else if(stage==1&&g_test_t>1.0f){shot("about_author");g_app.help_page=19;stage=2;g_test_t=0;}else if(stage==2&&g_test_t>1.0f){shot("about_references");mlog("TEST66 about visible author and references rendered");log_flush();stage=3;}return;}
    if(mode==65){static int asked,last=-1;if(!asked&&g_test_t>1.0f){asked=1;api_request_announcement_check();}int state=community_announcement_state();if(state!=last){last=state;mlog("TEST65 announcement state=%d",state);log_flush();}return;}
    if(mode==63){static int stage;if(!stage&&g_test_t>1.5f){stage=1;mlog("TEST63 start result=%d",mvx_start(990000004,"Export","Regression"));}else if(stage==1&&!mvx_active()){stage=2;mlog("TEST63 done result=%d percent=%d path=%s",mvx_result(),mvx_pct(),mvx_path());}return;}
    if(mode==62){
        static int stage,stop_after=8;
        if(!stage&&g_test_t>1.5f&&net_apctl_state()==4){
            char url[1000]={0};FILE*f=fopen(APP_DIR "mvtesturl.txt","r");
            if(f){fgets(url,sizeof(url),f);fclose(f);url[strcspn(url,"\r\n")]=0;}
            FILE*seektest=fopen(APP_DIR "mvtestseek.txt","r");if(seektest){char seconds[16]={0};fgets(seconds,sizeof(seconds),seektest);int requested=atoi(seconds);stop_after=requested>=20&&requested<=90?requested:20;fclose(seektest);}
            stage=1;g_test_t=0;
            if(!url[0]||mv_stream_start(url,APP_DIR "cache/mv_990000003_240.mp4")<0){mlog("TEST62 source-start failed");stage=4;}
        }else if(stage==1&&mv_stream_total()>0&&mv_stream_available()>=mv_stream_prefetch()){
            int result=mv_start_stream(APP_DIR "cache/mv_990000003_240.mp4","Software HTTP Test");
            mlog("TEST62 start result=%d available=%lld total=%lld",result,mv_stream_available(),mv_stream_total());
            if(!result){set_screen(SC_MV);stage=2;g_test_t=0;}else stage=4;
        }else if(stage==2&&g_test_t>(float)stop_after){mv_stop();stage=3;g_test_t=0;}
        else if(stage==3&&g_test_t>3.0f){mlog("TEST62 done available=%lld total=%lld screen=%d peak_kbps=%d",mv_stream_available(),mv_stream_total(),g_app.screen,net_peak_speed_kbps());stage=4;}
        return;
    }
    if (mode == 60) {
        /* 连播两次 MV（第二次进 MV 必须也出画面），每次 8 秒，最后看 UI 恢复 */
        static int s60, cyc;

        if (s60 == 0 && g_test_t > 1.5f) {
            s60 = 1; g_test_t = 0;
            g_app.has_now = 1;
            g_app.now.mv = 5361181;
            snprintf(g_app.now.name, sizeof(g_app.now.name), "MV Test %d", cyc + 1);
            mv_try_start();
        } else if (s60 == 1 && g_test_t > 8.0f) {
            s60 = 2; g_test_t = 0;
            mlog("TEST60 stopping mv cycle %d", cyc + 1);
            mv_stop();
        } else if (s60 == 2 && g_test_t > 2.5f) {
            g_test_t = 0;
            if (cyc == 0) {
                cyc = 1;
                mlog("TEST60 second mv cycle");
                g_app.has_now = 1;
                g_app.now.mv = 5361181;
                snprintf(g_app.now.name, sizeof(g_app.now.name), "MV Test 2");
                mv_try_start();
                s60 = 1;
            } else {
                s60 = 3;
                mlog("TEST60 done screen=%d", g_app.screen);
                shot("after_mv");
            }
        }
        return;
    }

    if (mode == 59) {
        /* VIP 歌走备用音源下载：进度条能不能从 0% 动起来 */
        static int s59, tick;

        if (s59 == 0 && g_test_t > 1.5f) {
            s59 = 1;
            g_test_t = 0;
            memset(&g_list, 0, sizeof(g_list));
            g_list.count = 1;
            g_list.kind = LK_SEARCH;
            g_list.songs[0].id = 1901371647;
            snprintf(g_list.songs[0].name, sizeof(g_list.songs[0].name), "孤勇者");
            snprintf(g_list.songs[0].artist, sizeof(g_list.songs[0].artist), "陈奕迅");
            g_list.songs[0].dur_ms = 256000;
            g_list.songs[0].fee = 1;
            mlog("TEST59 download vip");
            api_request_download(&g_list.songs[0]);
        } else if (s59 == 1 && g_test_t > 3.0f) {
            DlItem it[4];
            int n;

            g_test_t = 0;
            n = dl_queue_snapshot(it, 4);
            if (n > 0) {
                mlog("TEST59 dl n=%d id=%lld total=%d pct=%d state=%d", n, it[0].id,
                     it[0].total, it[0].pct, it[0].state);
            }
            if (++tick > 8) s59 = 2;
        }
        return;
    }

    if (mode == 58) {
        /* 真机/模拟器上试几种 sceDisplaySetFrameBuf 组合，看哪种 8888 能成功 */
        static int s58;

        if (s58 == 0 && g_test_t > 1.5f) {
            unsigned vram = (unsigned)sceGeEdramGetAddr();
            int r1, r2, r3, r4;

            s58 = 1;
            sceDisplayWaitVblankStart();
            r1 = sceDisplaySetFrameBuf((void *)vram, 512, PSP_DISPLAY_PIXEL_FORMAT_8888,
                                       PSP_DISPLAY_SETBUF_IMMEDIATE);
            sceDisplayWaitVblankStart();
            r2 = sceDisplaySetFrameBuf((void *)(vram + 0x40000000), 512,
                                       PSP_DISPLAY_PIXEL_FORMAT_8888,
                                       PSP_DISPLAY_SETBUF_IMMEDIATE);
            sceDisplayWaitVblankStart();
            r3 = sceDisplaySetFrameBuf((void *)(vram + 0x88000), 512,
                                       PSP_DISPLAY_PIXEL_FORMAT_8888,
                                       PSP_DISPLAY_SETBUF_IMMEDIATE);
            sceDisplayWaitVblankStart();
            r4 = sceDisplaySetFrameBuf((void *)vram, 512, PSP_DISPLAY_PIXEL_FORMAT_565,
                                       PSP_DISPLAY_SETBUF_IMMEDIATE);
            mlog("TEST58 setfb 0=%08X uncached=%08X off88000=%08X 565=%08X",
                 r1, r2, r3, r4);
        }
        return;
    }

    if (mode == 56) {
        /* 本地 mp3：没有内嵌封面时，联网匹配封面/歌词准不准 */
        static int s56;

        if (s56 == 0 && g_test_t > 1.5f) {
            s56 = 1; g_test_t = 0;
            local_rescan();
            mlog("TEST56 local count=%d", local_count());
            if (local_count() > 0) play_local_file(0);
        } else if (s56 == 1 && g_test_t > 8.0f) {
            s56 = 2; g_test_t = 0;
            mlog("TEST56 after: art=%d lyric=%d name=[%s] artist=[%s] dur=%d", 
                 api_art(NULL, NULL) != NULL, api_lyric()->count,
                 g_app.now.name, g_app.now.artist, audio_dur_ms());
            shot("local_match");
        }
        return;
    }

    if (mode == 57) {
        /* 我的页面：我喜欢显示的数量 */
        static int s57;

        if (s57 == 0 && g_test_t > 2.0f) {
            s57 = 1; g_test_t = 0;
            g_app.mine_sel = 0;
            set_screen(SC_MINE);
        } else if (s57 == 1 && g_test_t > 8.0f) {
            s57 = 2; g_test_t = 0;
            mlog("TEST57 liked_total=%d logged=%d", api_liked_total(), api_logged_in());
            shot("mine_liked");
        }
        return;
    }

    if (mode == 52) {
        /* 设置页 / 备用音源页截图巡检 */
        static int s52;

        if (s52 == 0 && g_test_t > 1.0f) {
            s52 = 1; g_test_t = 0; set_screen(SC_SETTINGS);
        } else if (s52 == 1 && g_test_t > 2.0f) {
            s52 = 2; g_test_t = 0; g_app.settings_sel = 11;
        } else if (s52 == 2 && g_test_t > 2.0f) {
            s52 = 3; g_test_t = 0; g_app.alt_sel = 0; set_screen(SC_ALT);
        } else if (s52 == 3 && g_test_t > 2.0f) {
            s52 = 4; g_test_t = 0;
            mlog("TEST52 alt byfuns=%d kuwo=%d gateway=%d gd=%d gw=[%s]",
                 alt_enabled(ALT_BYFUNS), alt_enabled(ALT_KUWO), alt_enabled(ALT_GATEWAY),
                 alt_enabled(ALT_GDSTUDIO), alt_gateway());
            shot("alt_page");
        }
        return;
    }

    if (mode == 53) {
        /* VIP 歌：官方只给试听 -> 备用音源能否拿到整首 */
        static int s53;

        if (s53 == 0 && g_test_t > 1.0f) {
            memset(&g_list, 0, sizeof(g_list));
            g_list.count = 1;
            g_list.kind = LK_SEARCH;
            snprintf(g_list.title, sizeof(g_list.title), "备用音源测试");
            g_list.songs[0].id = 1901371647;
            snprintf(g_list.songs[0].name, sizeof(g_list.songs[0].name), "孤勇者");
            snprintf(g_list.songs[0].artist, sizeof(g_list.songs[0].artist), "陈奕迅");
            g_list.songs[0].dur_ms = 256000;
            g_list.songs[0].fee = 1;
            s53 = 1; g_test_t = 0;
            mlog("TEST53 play vip song");
            play_index(0);
        } else if (s53 == 1 && g_test_t > 20.0f) {
            s53 = 2; g_test_t = 0;
            mlog("TEST53 st=%d pos=%d dur=%d err=%s", audio_state(), audio_pos_ms(),
                 audio_dur_ms(), audio_error());
            shot("vip_alt");
        }
        return;
    }

    if (mode == 54) {
        /* 播放页截图（用缓存里的第一首，离线也能画） */
        static int s54;

        if (s54 == 0 && g_test_t > 1.5f) {
            s54 = 1; g_test_t = 0;
            mlog("TEST54 cache count=%d", cache_count());
            play_cache_entry(0);
        } else if (s54 == 1 && g_test_t > 4.0f) {
            s54 = 2; g_test_t = 0;
            mlog("TEST54 player st=%d pos=%d art=%d lyric=%d", audio_state(),
                 audio_pos_ms(), api_art(NULL, NULL) != NULL, api_lyric()->count);
            shot("player_page");
            g_app.comment_from = SC_PLAYER;
            api_request_comments(g_app.now.id, 0);
        } else if (s54 == 2 && g_test_t > 6.0f) {
            s54 = 3; g_test_t = 0;
            mlog("TEST54 comments=%d total=%d screen=%d", api_comments()->count,
                 api_comment_total(), g_app.screen);
            shot("comments_page");
        }
        return;
    }

    if (mode == 51) {
        /* 歌单分页：加载「我喜欢的音乐」，滚到底触发下一页 */
        static int s51;

        if (s51 == 0 && g_test_t > 1.0f) {
            s51 = 1;
            g_test_t = 0;
            api_request_liked();
        } else if (s51 == 1 && g_test_t > 8.0f && g_list.count > 0) {
            s51 = 2;
            g_test_t = 0;
            mlog("TEST51 liked=%d total=%d ids=%d", g_list.count, g_app.list_total,
                 api_playlist_count());
            g_app.list_sel = g_list.count - 2;
        } else if (s51 == 2 && g_test_t > 7.0f) {
            s51 = 3;
            g_test_t = 0;
            mlog("TEST51 after page=%d total=%d more=%d ids=%d", g_list.count,
                 g_app.list_total, g_app.list_more, api_playlist_count());
            shot("liked_page");
        }
        return;
    }

    if (mode == 23) {
        /* 放不了的歌要能自动跳下一首 */
        static int s23;
        if (s23 == 0 && g_test_t > 1.0f) {
            s23 = 1; g_test_t = 0; api_request_daily();
        } else if (s23 == 1 && g_test_t > 6.0f && g_list.count > 0) {
            int bad = -1;

            for (int i = 0; i < g_list.count; ++i)
                if (g_list.songs[i].playable == 2) { bad = i; break; }
            s23 = 2; g_test_t = 0;
            mlog("TEST23 daily=%d bad=%d", g_list.count, bad);
            play_index(bad >= 0 ? bad : 0);
        } else if (s23 == 2 && g_test_t > 14.0f) {
            s23 = 3; g_test_t = 0;
            mlog("TEST23 after play: st=%d pos=%d idx=%d skip=%d err=%s",
                 audio_state(), audio_pos_ms(), g_app.playing_index,
                 g_auto_skip, audio_error());
            shot("autoskip");
        }
        return;
    }

    if (mode == 22) {
        /* 社区广场 + 每日推荐头 + 设置页 */
        static int s22;

        if (s22 == 0 && g_test_t > 1.0f) {
            s22 = 1; g_test_t = 0;
            g_app.grid_kind = 4;
            api_request_cat_lists(cat_names[0]);
        } else if (s22 == 1 && g_test_t > 7.0f) {
            s22 = 2; g_test_t = 0;
            mlog("TEST22 square cards=%d screen=%d", api_cards()->count, g_app.screen);
            shot("square");
            api_request_daily();
        } else if (s22 == 2 && g_test_t > 5.0f) {
            s22 = 3; g_test_t = 0;
            shot("daily2");
            set_screen(SC_SETTINGS);
            g_app.settings_sel = 6;
        } else if (s22 == 3 && g_test_t > 3.0f) {
            s22 = 4; g_test_t = 0;
            shot("set");
        }
        return;
    }

    if (mode == 21) {
        /* 我喜欢的 / 最近播放 解析巡检 */
        static int s21;

        if (s21 == 0 && g_test_t > 1.0f) {
            s21 = 1; g_test_t = 0;
            mlog("TEST21 liked go uid=%lld logged=%d", api_login_uid(), api_logged_in());
            api_request_liked();
        } else if (s21 == 1 && g_test_t > 6.0f) {
            s21 = 2; g_test_t = 0;
            mlog("TEST21 liked count=%d kind=%d title=%s", g_list.count, g_list_kind, g_list_title);
            shot("liked2");
            api_request_played();
        } else if (s21 == 2 && g_test_t > 8.0f) {
            s21 = 3; g_test_t = 0;
            mlog("TEST21 played count=%d kind=%d", g_list.count, g_list_kind);
            shot("played");
        }
        return;
    }

    if (mode == 20) {
        /* 回归：歌单详情 -> 从歌单里播第一首，看会不会卡死 */
        static int s20;

        if (s20 == 0 && g_test_t > 1.0f) {
            s20 = 1; g_test_t = 0; api_request_playlist(3778678);
        } else if (s20 == 1 && g_test_t > 7.0f) {
            s20 = 2; g_test_t = 0;
            mlog("TEST20 playlist=%d screen=%d", g_list.count, g_app.screen);
            shot("pl");
            play_index(0);
        } else if (s20 == 2 && g_test_t > 10.0f) {
            s20 = 3; g_test_t = 0;
            mlog("TEST20 play st=%d pos=%d sid=%lld", audio_state(), audio_pos_ms(), g_app.now.id);
            shot("plplay");
            set_screen(SC_LIST);
        } else if (s20 == 3 && g_test_t > 3.0f) {
            s20 = 4; g_test_t = 0;
            shot("plhead");
            api_request_comments(g_app.now.id, 0);
        } else if (s20 == 4 && g_test_t > 6.0f) {
            s20 = 5; g_test_t = 0;
            mlog("TEST20 comments n=%d total=%d", api_comments()->count, api_comment_total());
            shot("comments2");
        }
        return;
    }

    if (mode == 19) {
        /* v3 巡检：每日推荐 -> 推荐歌单 -> 评论 -> 我喜欢 */
        static int s19;

        if (s19 == 0 && g_test_t > 1.0f) {
            s19 = 1;
            g_test_t = 0;
            api_request_daily();
        } else if (s19 == 1 && g_test_t > 4.0f) {
            s19 = 2;
            g_test_t = 0;
            mlog("TEST19 daily list=%d screen=%d", g_list.count, g_app.screen);
            shot("daily");
            g_app.grid_sel = 0;
            g_app.grid_page = 0;
            api_request_recommend_lists();
        } else if (s19 == 2 && g_test_t > 5.0f) {
            s19 = 3;
            g_test_t = 0;
            mlog("TEST19 grid cards=%d screen=%d", api_cards()->count, g_app.screen);
            shot("grid");
            api_request_artist_top();
        } else if (s19 == 3 && g_test_t > 5.0f) {
            s19 = 4;
            g_test_t = 0;
            mlog("TEST19 artist cards=%d", api_cards()->count);
            shot("artist");
            api_request_comments(g_app.now.id, 0);
        } else if (s19 == 4 && g_test_t > 4.0f) {
            s19 = 5;
            g_test_t = 0;
            mlog("TEST19 comments n=%d total=%d screen=%d", api_comments()->count,
                 api_comment_total(), g_app.screen);
            shot("comments");
            api_request_liked();
        } else if (s19 == 5 && g_test_t > 6.0f) {
            s19 = 6;
            g_test_t = 0;
            mlog("TEST19 liked=%d", g_list.count);
            shot("liked");
        }
        return;
    }

    if (mode == 18) {
        /* 登录页巡检：进账号页 -> 取二维码 -> 截图（之后用 PC 解码验证能被扫） */
        static int step18;

        if (step18 == 0 && g_test_t > 1.0f) {
            step18 = 1;
            g_test_t = 0;
            set_screen(SC_LOGIN);
            g_qr_poll_t = 0;
            if (!api_logged_in() && api_qr_state() == 0) api_request_qr_new();
            else if (api_logged_in()) api_request_account();
        } else if (step18 == 1 && g_test_t > 3.0f) {
            int dim = 0;

            step18 = 2;
            g_test_t = 0;
            api_qr_matrix(&dim);
            mlog("TEST18 qr state=%d dim=%d url=%s", api_qr_state(), dim, api_qr_url());
            shot("login");
        }
        return;
    }

    if (mode == 17) {
        /* 分页巡检：搜一次，然后连拉两页，日志里看数量是否累加 */
        static int step17;

        if (step17 == 0 && g_test_t > 1.0f) {
            step17 = 1;
            g_test_t = 0;
            search_do("zhoujielun");
        } else if (step17 == 1 && g_app.search_state == 2 && g_test_t > 2.0f) {
            step17 = 2;
            g_test_t = 0;
            mlog("TEST17 page1=%d total=%d", g_list.count, g_app.list_total);
            g_app.list_more = 1;
            api_request_search_more();
        } else if (step17 == 2 && g_app.list_more == 0 && g_test_t > 2.0f) {
            step17 = 3;
            g_test_t = 0;
            mlog("TEST17 page2=%d", g_list.count);
            g_app.list_more = 1;
            api_request_search_more();
        } else if (step17 == 3 && g_app.list_more == 0 && g_test_t > 2.0f) {
            step17 = 4;
            g_test_t = 0;
            mlog("TEST17 page3=%d total=%d", g_list.count, g_app.list_total);
        }
        return;
    }

    if (mode == 16) {
        /* 联想面板巡检：填好拼音 -> 等防抖 -> 截图 -> 进入选择态再截一张 */
        static int step16;

        if (step16 == 0 && g_test_t > 1.0f) {
            step16 = 1;
            g_test_t = 0;
            g_app.kb_edit = 1;
            g_app.kb_row = 0;
            g_app.kb_col = 0;
            snprintf(g_app.query, sizeof(g_app.query), "zhoujielun");
            g_app.query_len = (int)strlen(g_app.query);
            set_screen(SC_SEARCH);
        } else if (step16 == 1 && g_test_t > 2.5f) {
            step16 = 2;
            g_test_t = 0;
            mlog("TEST16 suggest n=%d state=%d", api_suggest_count(), g_app.sug_state);
            shot("sug");
        } else if (step16 == 2 && g_test_t > 1.5f) {
            step16 = 3;
            g_test_t = 0;
            g_app.sug_focus = 1;
            g_app.sug_sel = 1;
            shot("sug_sel");
        }
        return;
    }

    if (mode == 15) {
        /* 本地播放 + 连续快进：验证 decode/out 双线程 + PCM 环 + seek 握手 */
        static int step15;
        static int seeks15;

        if (step15 == 0) {
            step15 = 1;
            g_test_t = 0;
            local_rescan();
            if (local_count() > 0) {
                play_local_file(0);
                mlog("TEST15 play local 0 %s", local_path_of(0));
            } else {
                mlog("TEST15 no local file");
            }
        } else if (step15 == 1 && g_test_t > 2.5f) {
            int sc = 0, sm = 0;

            g_test_t = 0;
            audio_stall_stats(&sc, &sm);
            if (seeks15 < 5) {
                seeks15++;
                mlog("TEST15 seek #%d from %dms state=%d starve=%d", seeks15, audio_pos_ms(),
                     audio_state(), sc);
                audio_seek(audio_pos_ms() + 15000);
            } else if (seeks15 == 5) {
                seeks15++;
                mlog("TEST15 seek done pos=%dms state=%d err=%s starve=%d maxgap=%dms",
                     audio_pos_ms(), audio_state(), audio_error(), sc, sm);
            } else {
                mlog("TEST15 final pos=%dms state=%d starve=%d", audio_pos_ms(), audio_state(), sc);
            }
        }
        return;
    }

    if (mode == 13) {
        /* 本地音乐 + 导出测试 */
        static int step;
        static long long exp_id;

        if (step == 0) {
            step = 1;
            g_test_t = 0;
            local_rescan();
            g_app.cache_tab = 0;
            set_screen(SC_CACHE);
            mlog("TEST13 local count=%d", local_count());
            for (int i = 0; i < local_count(); ++i)
                mlog("TEST13 local[%d] %s %dKB", i, local_path_of(i), local_size(i) / 1024);
        } else if (step == 1 && g_test_t > 1.2f) {
            step = 2;
            g_test_t = 0;
            g_app.cache_tab = 1;
            mlog("TEST13 switch to local tab (count=%d)", cache_count());
        } else if (step == 2 && g_test_t > 1.2f) {
            step = 3;
            g_test_t = 0;
            if (local_count() > 0) {
                int picked = -1;

                for (int i = 0; i < local_count(); ++i) {
                    int ar = api_local_art(local_path_of(i));

                    mlog("TEST13 art[%d] -> %d", i, ar);
                    if (ar == 0 && picked < 0) picked = i;
                }
                if (picked >= 0) {
                    play_local_file(picked);
                    mlog("TEST13 play local %d", picked);
                }
            } else {
                mlog("TEST13 no local file");
            }
        } else if (step == 3 && g_test_t > 3.0f) {
            step = 4;
            g_test_t = 0;
            mlog("TEST13 local play state=%d pos=%d dur=%d err=%s", audio_state(), audio_pos_ms(),
                 audio_dur_ms(), audio_error());
        } else if (step == 4 && g_test_t > 2.0f) {
            step = 5;
            g_test_t = 0;
            audio_stop();               /* 导出前先停播，避免文件被占用 */
            if (cache_count() > 0) {
                const CacheEntry *e = cache_get(0);
                Song sg;

                memset(&sg, 0, sizeof(sg));
                exp_id = e->id;
                sg.id = e->id;
                sg.dur_ms = e->dur_ms;
                snprintf(sg.name, sizeof(sg.name), "%s", e->name);
                snprintf(sg.artist, sizeof(sg.artist), "%s", e->artist);
                snprintf(sg.album, sizeof(sg.album), "%s", e->album);
                mlog("TEST13 export %lld", exp_id);
                api_request_export(&sg);
            }
        } else if (step == 5 && g_test_t > 6.0f) {
            SceIoStat st;
            char path[128];

            step = 6;
            g_test_t = 0;
            snprintf(path, sizeof(path), "ms0:/MUSIC/MELODY/MELODY-%lld.mp3", exp_id);
            if (sceIoGetstat(path, &st) >= 0)
                mlog("TEST13 exported ok %s %dKB", path, (int)(st.st_size / 1024));
            else
                mlog("TEST13 export missing %s (result=%d)", path, local_export_result());
        }
        return;
    }

    if (mode == 12) {
        /* 结尾测试：跳到离结尾 5 秒，看是否正常结束并自动下一首 */
        static int phase;

        if (phase == 0 && audio_state() == AUDIO_PLAY) {
            phase = 1;
            g_test_t = 0;
            mlog("TEST seek near end: pos=%d dur=%d", audio_pos_ms(), audio_dur_ms());
            audio_seek(audio_dur_ms() - 5000);
        } else if (phase == 1 && g_test_t > 2.0f) {
            g_test_t = 0;
            mlog("TEST near-end state=%d pos=%d/%d err=%s", audio_state(), audio_pos_ms(),
                 audio_dur_ms(), audio_error());
        }
        return;
    }

    if (mode == 11) {
        /* 连续切歌：验证「播放中选别的歌」不会卡、能切过去 */
        static int idx;

        if (g_app.screen == SC_PLAYER && audio_state() == AUDIO_PLAY && g_test_t > 7.0f) {
            g_test_t = 0;
            idx++;
            if (idx < 6 && idx < g_list.count) {
                mlog("TEST switch to song %d", idx);
                play_index(idx);
            } else {
                mlog("TEST switch done");
            }
        }
        return;
    }

    if (mode == 9) {
        /* 退出测试：6 秒后走完整关停流程 */
        if (g_test_step <= 0 && t > 6.0f) {
            g_test_step = 1;
            mlog("TEST exit begin");
            g_exit_req = 1;
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* 退出回调                                                            */
/* ------------------------------------------------------------------ */

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    g_exit_req = 1;
    /* 真机 6.60 PRO-C2：HOME → 退出时主线程有时停在系统挂起状态，
     * 永远走不到主循环里的 g_exit_req 处理，系统就一直停在"请稍后"。
     * 这里给主线程 500ms 做正常收尾；超时就由回调线程直接 ExitGame，
     * 保证系统退出流程一定能完成。 */
    sceKernelDelayThread(500 * 1000);
    log_flush();
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int cbid;

    (void)args;
    (void)argp;

    cbid = sceKernelCreateCallback("MELODY exit", exit_callback, NULL);
    if (cbid >= 0) sceKernelRegisterExitCallback(cbid);

    sceKernelSleepThreadCB();
    return 0;
}

static int setup_callbacks(void)
{
    int thid = sceKernelCreateThread("melody_cb", callback_thread, 0x11, 0x2000, 0, NULL);

    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
    return thid;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int privacy_gate(void){
    if(community_privacy_accepted())return 1;
    mlog("privacy gate ready: network not initialized");log_flush();
    unsigned previous=0;int scroll=0;
    while(!g_exit_req){SceCtrlData pad={0};sceCtrlReadBufferPositive(&pad,1);unsigned held=pad.Buttons;
        if(pad.Ly<70)held|=PSP_CTRL_UP;else if(pad.Ly>186)held|=PSP_CTRL_DOWN;
        unsigned pressed=held&~previous;previous=held;
        if(pressed&PSP_CTRL_UP){if(scroll>0)--scroll;}
        if(pressed&PSP_CTRL_DOWN){if(scroll<40)++scroll;}
        if(pressed&PSP_CTRL_CROSS)return 0;
        if(pressed&PSP_CTRL_CIRCLE){if(community_privacy_accept()==0)return 1;ui_toast("接受记录保存失败，请检查记忆棒");}
        ui_document("隐私政策 / 首次启动",community_privacy_text(),scroll,"○ 接受并继续    × 拒绝并退出");gfx_flip();
    }return 0;
}
int app_main(void)
{
    SceCtrlData pad;
    int running = 1;
    int last_screen = -1;
    int forced = 0;
    float last_t = 0;

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    setup_callbacks();

    gfx_init();
    log_open();
    mlog("MELODY PSP %s start %s %s", APP_VER, __DATE__, __TIME__);
    if (playback_context_init() < 0) {
        log_close();
        sceKernelExitGame();
        return -1;
    }
    update_apply_startup();

    g_start_us = sceKernelGetSystemTimeWide();
    scePowerSetClockFrequency(333, 333, 166);

    /* PSP 的 MP3 解码器对非 44.1kHz 的流有兼容开关：只有声明的 SDK
     * 版本够新才放行。声明成 6.00，48kHz 的网易云 MP3 才能解码。 */
    sceKernelSetCompiledSdkVersion(0x06000000);
    mlog("compiled sdk = 0x%08X", sceKernelGetCompiledSdkVersion());
    mlog("devkit=0x%08X clock=%dMHz bus=%dMHz", sceKernelDevkitVersion(),
         scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());

    memset(&g_app, 0, sizeof(g_app));
    memset(&g_list, 0, sizeof(g_list));
    g_app.screen = SC_BOOT;
    g_app.wifi_state = -1;
    g_app.kb_row = -1;
    g_app.kb_col = 0;
    g_app.kb_edit = 1;
    snprintf(g_app.net_msg, sizeof(g_app.net_msg), "正在初始化");

    ui_init();
    if(!privacy_gate()){mlog("privacy declined: exit before audio/network/API initialization");log_close();sceKernelExitGame();return 0;}
    /* Reserve the idle stream worker before optional high-address GUI caches.
     * A late64KiB stack otherwise pins the gap after those caches are freed. */
    if(mv_stream_prepare_worker()<0)mlog("mv stream: early worker reservation failed");
    if (g_update_applied) ui_toast("更新已安装，退出再进一次");
    update_gallery_load();
    store_init();
    cache_init();
    g_app.quality = store_quality();
    g_app.show_mem = store_show_mem();
    g_app.theme = store_theme();
    g_app.dl_parts = store_dl_parts();
    if (g_app.dl_parts < 2) g_app.dl_parts = 4;
    if (g_app.theme < 0 || g_app.theme > 1) g_app.theme = 0;
    ui_theme_changed();
    /* Restore the global renderer before audio/network allocations; keep the
     * saved preference on failure so the next launch can retry it. */
    if(gfx_set_renderer(store_renderer())<0){
        const char *err=gfx_renderer_error();
        ui_toast(err&&err[0]?err:"界面渲染初始化失败，使用 CPU");
    }

    audio_set_volume(g_volume);
    if (audio_init() < 0) mlog("audio_init failed");
    dl_init();
    dl_set_parts(g_app.dl_parts);
    if (net_init() < 0) mlog("net_init failed");
    api_init();

    mlog("free memory = %dKB (max block %dKB)",
         (int)(sceKernelTotalFreeMemSize() / 1024), (int)(sceKernelMaxFreeMemSize() / 1024));

    snprintf(g_app.net_msg, sizeof(g_app.net_msg), "正在连接网络");

    /* 启动时仍按住的键（比如 XMB 里按 ○ 进游戏还没松手）不算「按下」，
     * 否则开机第一帧就会误开 WiFi 页。 */
    {
        SceCtrlData pad0;

        if (sceCtrlPeekBufferPositive(&pad0, 1) > 0) g_prev_btn = pad0.Buttons;
    }

    if (dev_mode() == 5) {
        open_wifi_screen();
    } else if (dev_mode() != 50) {
        api_request_probe();
    } else {
        mlog("dev50: skip probe");
    }
    if (dev_mode() == 40) api_request_update_check();

    while (running) {
        float t = now_sec();
        float dt = t - last_t;
        unsigned int pressed, held;

        last_t = t;
        if (dt < 0) dt = 0;
        if (dt > 0.1f) dt = 0.1f;
        g_app.time = t;
        g_app.spinner += dt * 2.0f;

        sceCtrlPeekBufferPositive(&pad, 1);
        held = pad.Buttons;
        /* Analogue dead zone; use the existing directional repeat/navigation. */
        if(pad.Lx<70)held|=PSP_CTRL_LEFT;else if(pad.Lx>186)held|=PSP_CTRL_RIGHT;
        if(pad.Ly<70)held|=PSP_CTRL_UP;else if(pad.Ly>186)held|=PSP_CTRL_DOWN;
        const unsigned int sampled_held=held;
        pressed = held & ~g_prev_btn;
        g_prev_btn = held;

        /* 上一帧绘制期间按下的键（可能已经松开）也要算一次按下 */
        if (g_pending_press) {
            pressed |= g_pending_press;
            g_pending_press = 0;
        }

        pump_events();

        /* 启动静默检查发现新版本：回到主页就弹窗（只弹一次，按过"稍后"不再弹） */
        if (g_app.screen == SC_HOME && !g_app.upd_dlg && !g_app.upd_dismissed &&
            api_update_state() == 3) {
            g_app.upd_dlg = 1;
            g_app.upd_sel = 1;
            g_app.upd_scroll = 0;
            mlog("update: dialog v%s", api_update_version());
        }

        if(api_logged_in()&&g_app.has_now){int liked=api_song_liked(g_app.now.id);if(liked>=0)g_app.is_liked=liked;}
        net_speed_tick();
        prefetch_tick();
        g_app.busy = api_busy() ? 1 : 0;

        /* apctl 是内核调用，别每帧问 */
        if ((g_frame & 63) == 0) {
            char name[40];

            if (net_apctl_state() == 6 && net_current_name(name, sizeof(name)) == 0) {
                g_app.wifi_state = 0;
                snprintf(g_app.wifi_name, sizeof(g_app.wifi_name), "%s", name);
            } else if (!g_app.online) {
                g_app.wifi_state = 1;
            }
        }

        if ((g_frame & 15) == 0) {
            int s = net_wlan_switch();
            if (s >= 0) g_app.wlan_on = s;

            if (cache_gen() != g_cache_gen_seen) {
                g_cache_gen_seen = cache_gen();
                if (g_app.cache_sel >= cache_count()) g_app.cache_sel = 0;
            }
            if (dl_queue_gen() != g_dl_gen_seen) {
                g_dl_gen_seen = dl_queue_gen();
                if(g_app.screen==SC_DOWNLOAD&&g_app.dl_tab==0)download_follow_focus();
                else if(g_app.dl_sel>=dl_queue_count())g_app.dl_sel=0;
            }
        }

        if (g_app.toast_t > 0) g_app.toast_t -= dt;

        /* Hold music policy state until the MV owner has been safely joined. */
        int mv_has_owner = g_mv_ui_retained || g_mv_connecting || mv_active() || mv_finished();
        dl_set_video_priority(mv_has_owner);
        if (mv_has_owner || audio_needs_restart()) {
            /* MV/fault ownership time must never age a music stall window. */
            g_auto_wait_active = g_auto_wait_fired = 0;
        }
        if (!mv_has_owner && !audio_needs_restart()) {
        /* 播完自动下一首 */
        /* 播完自动下一首：只触发一次（下一首可能放不了，状态会一直停在 END） */
        if (audio_state() == AUDIO_END && g_app.has_now) {
            int dur = audio_dur_ms();

            if (!g_end_done) {
                g_end_done = 1;
                g_app.paused = 1;
                if (dur > 0 && audio_pos_ms() > dur - 3000) {
                    if (g_app.play_mode == 2) {
                        audio_seek(0);
                        audio_pause(0);
                        g_app.paused = 0;
                    } else {
                        preserve_play_page=1;play_next(1);preserve_play_page=0;
                    }
                }
                mlog("auto next (end of song) pos=%d dur=%d ctx=%d/%d src=%d", (int)audio_pos_ms(),
                     dur, g_playback.index, g_playback.count, g_playback.source);
            }
        } else {
            g_end_done = 0;
        }

        if (audio_state() == AUDIO_ERROR && g_app.has_now && !g_retry_gave_up &&
            !g_retry_waiting && !g_app.busy && g_retry_t<=0 && g_app.now.id > 0) {
            /* 瞬时错误（网络抖动、文件刚写完还没刷 FAT）：自己重试两次。
             * 本地歌 (id<=0) 没有直链可请求，直接报错，别走在线重试链 */
            if (g_retry_n < 2) {
                g_retry_n++;
                g_retry_t = 1.5f;
                ui_toast("播放出错，正在重试");
                /* The reason matters: 没有数据 / 下载中断 / 解码器初始化失败 are
                 * different bugs. Without it the log only says "retry 1". */
                mlog("audio error, retry %d: %s", g_retry_n, audio_error());
            } else {
                /* 重试用完：像网易云那样直接跳下一首（最多连跳 5 首），
                 * 别把播放器钉死在 0:00 转圈。 */
                g_retry_gave_up = 1;
                api_note_play_fail();          /* 403 多半是高音质没权限，下次用 128k */
                mlog("audio error, giving up: %s", audio_error());
                g_app.paused = 1;
                g_retry_t = 0;
                audio_pause(1);
                ui_toast("播放连接失败，已暂停；按○重试，不自动跳歌");
            }
            g_prev_btn = held;
        }

        /* 播放稳定 2 秒以上才认为这次起播成功；否则重试计数不清零。
         * 以前 EV_PLAY_OK 就清 g_retry_n，而起播失败在 1~3 秒内就报错，
         * 于是永远停在 "retry 1" 循环（日志里连续三条 retry 1）。 */
        if (audio_state() == AUDIO_PLAY && audio_pos_ms() > 2000) {
            g_play_stable_ms = audio_pos_ms();
            if (g_retry_n) g_retry_n = 0;
        } else if (audio_state() != AUDIO_PLAY) {
            g_play_stable_ms = 0;
        }

        if (g_retry_t > 0) {
            g_retry_t -= dt;
            if (g_retry_t <= 0 && g_app.has_now && g_app.now.id > 0) {
                mlog("retry play");
                g_retry_waiting=1;
                api_request_play(g_app.playing_index, &g_app.now);
            }
        }

        /* 缓存已经完整、播放却没起来（边下边播卡在缓冲 / 报错过）：
         * 直接改用缓存文件重开播，不用用户按 X 再按 O。
         * 刚快进/快退过的那几秒不算：seek 时解码器本来就会短暂进 BUFFER，
         * 以前会把它误判成卡死，直接重开播 → 表现就是「快进回到开头」。 */
        if (g_auto_fix_id != g_app.now.id || !g_app.has_now) {
            g_auto_fix_id = g_app.now.id;
            g_auto_fix_n = 0;
            g_auto_fix_t = 0;
            g_auto_wait_active = g_auto_wait_fired = 0;
        }
        if (g_app.has_now && g_app.now.id > 0 && !g_app.paused &&
            g_app.time - g_seek_guard_t > 3.0f &&
            (audio_state() == AUDIO_BUFFER || audio_state() == AUDIO_ERROR) &&
            cache_has(g_app.now.id)) {
            int pos = audio_pos_ms();
            /* Normal decoder startup briefly reports BUFFER. Require an actual
             * continuous stall, reset on any progress (including a seek), and
             * issue only once until progress or a non-stalled state is seen. */
            if (!g_auto_wait_active || g_auto_wait_pos != pos) {
                g_auto_wait_active = 1;
                g_auto_wait_fired = 0;
                g_auto_wait_pos = pos;
                g_auto_wait_t = g_app.time;
            }
            if (!g_auto_wait_fired && g_app.time - g_auto_wait_t > 2.5f &&
                g_auto_fix_n < 4 && g_app.time - g_auto_fix_t > 2.5f) {
                g_auto_wait_fired = 1;
                g_auto_fix_t = g_app.time;
                g_auto_fix_n++;
                mlog("auto: cache ready, restart playback (try %d) state=%d", g_auto_fix_n,
                     audio_state());
                api_request_play(g_app.playing_index, &g_app.now);
            }
        } else {
            /* Pause, seek grace and normal playback interrupt the wait. */
            g_auto_wait_active = g_auto_wait_fired = 0;
        }

        } /* !mv_has_owner: retain retry timers and END/error bookkeeping */

        {static int seen_export,last_export_pct=-1;int busy=mvx_active();
         if(busy){seen_export=1;int pct=mvx_pct();if(pct/20!=last_export_pct/20){char msg[64];snprintf(msg,sizeof(msg),"MV导出 %d%%",pct);ui_toast(msg);last_export_pct=pct;}}
         else if(seen_export){ui_toast(mvx_result()==0?"MV已导出到 ms0:/VIDEO":mvx_result()==-2?"导出失败：记忆棒空间不足":"MV导出失败或已取消");seen_export=0;last_export_pct=-1;}}
        auto_test(dt);

        if (dev_mode() == 42) {
            static int mv_test_done;

            if (!mv_test_done && g_list.count > 0) {
                for (int i = 0; i < g_list.count; ++i) {
                    if (g_list.songs[i].mv > 0) {
                        mlog("TEST mv song i=%d id=%lld mv=%d", i,
                             g_list.songs[i].id, g_list.songs[i].mv);
                        api_request_mv_detail(g_list.songs[i].mv);
                        mv_test_done = 1;
                        break;
                    }
                }
                if (!mv_test_done) {
                    mv_test_done = 1;
                    mlog("TEST no mv in list");
                }
            }
        }

        if (dev_mode() == 43) {
            static int mv_dl_started, mv_dl_logged;

            if (!mv_dl_started && g_list.count > 0) {
                for (int i = 0; i < g_list.count; ++i) {
                    if (g_list.songs[i].mv > 0) {
                        mlog("TEST mv dl start mv=%d", g_list.songs[i].mv);
                        api_request_mv_download(g_list.songs[i].mv);
                        mv_dl_started = 1;
                        break;
                    }
                }
                if (!mv_dl_started) {
                    mv_dl_started = 1;
                    mlog("TEST no mv in list");
                }
            }
            if (mv_dl_started && !mv_dl_logged && api_mv_download_state() == 2) {
                mv_dl_logged = 1;
                mlog("TEST mv dl done");
            }
        }

        if (dev_mode() == 44) {
            static int mv_play_started, mv_play_queued;

            if (!mv_play_started && g_list.count > 0) {
                for (int i = 0; i < g_list.count; ++i) {
                    int mv = g_list.songs[i].mv;
                    char path[128];

                    if (mv <= 0) continue;
                    snprintf(path, sizeof(path),
                             "./cache/mv_%d_240.mp4", mv);
                    {
                        SceIoStat st;

                        if (sceIoGetstat(path, &st) >= 0) {
                            if (mv_start(mv, path, g_list.songs[i].name) == 0) {
                                mlog("TEST mv play mvid=%d path=%s", mv, path);
                                set_screen(SC_MV);
                                mv_play_started = 1;
                            }
                        } else if (!mv_play_queued) {
                            mlog("TEST mv dl mvid=%d", mv);
                            api_request_mv_download(mv);
                            mv_play_queued = 1;
                        }
                    }
                    break;
                }
            }
        }

        if (dev_mode() == 45) {
            static int mv_audio_started, mv_audio_tried;

            if (!mv_audio_started && g_list.count > 0) {
                for (int i = 0; i < g_list.count; ++i) {
                    if (g_list.songs[i].mv > 0) {
                        mlog("TEST mv audio play i=%d mv=%d", i, g_list.songs[i].mv);
                        play_index(i);
                        mv_audio_started = 1;
                        break;
                    }
                }
            }
            if (mv_audio_started && !mv_audio_tried && audio_state() == AUDIO_PLAY) {
                mv_audio_tried = 1;
                mlog("TEST mv audio start");
                mv_try_start();
            }
        }

        if (dev_mode() == 46) {
            static int mv_menu_tried;

            if (!mv_menu_tried && g_list.count > 0) {
                for (int i = 0; i < g_list.count; ++i) {
                    if (g_list.songs[i].mv > 0) {
                        g_app.now = g_list.songs[i];
    g_app.is_liked = 0;
                        g_app.has_now = 1;
                        mlog("TEST mv menu mvid=%d", g_app.now.mv);
                        mv_try_start();
                        mv_menu_tried = 1;
                        break;
                    }
                }
            }
        }

        if (dev_mode() == 47) {
            static int dl_mv_done;

            if (!dl_mv_done && g_list.count > 0) {
                g_app.dl_tab = 1;
                g_app.dl_sel = 0;
                set_screen(SC_DOWNLOAD);
                dl_mv_done = 1;
            }
        }

        if (dev_mode() == 48) {
            static int list_mv_done;

            if (!list_mv_done && g_list.count > 0) {
                set_screen(SC_LIST);
                list_mv_done = 1;
            }
        }

        if (dev_mode() == 50) {
            static int mv_direct;
            static int mv_dumped;

            if (!mv_direct) {
                mv_direct = 1;
                g_app.has_now = 1;
                g_app.now.mv = 5361181;
                snprintf(g_app.now.name, sizeof(g_app.now.name), "MV Test");
                mlog("TEST mv direct");
                mv_try_start();
            }
            if (mv_direct && !mv_dumped && g_app.screen == SC_MV && g_app.time > 6.0f) {
                mv_dumped = 1;
                sceIoMkdir(APP_DIR "shots", 0777);
                mlog("TEST mv dump pos=%dms", mv_position_ms());
                if (gfx_dump_mv_bmp(APP_DIR "shots/mv_frame.bmp") == 0)
                    mlog("TEST mv frame dumped");
                else
                    mlog("TEST mv frame dump failed");
            }
            if (mv_dumped == 1 && g_app.time > 6.25f) {
                mv_dumped = 2;
                gfx_dump_mv_bmp(APP_DIR "shots/mv_frame2.bmp");
                mlog("TEST mv frame dumped 2");
            }
        }

        if (dev_mode() == 40) {
            static int auto_dl, auto_done;

            if (!auto_dl && api_update_state() == 3) {
                auto_dl = 1;
                api_request_update_download();
            }
            if (!auto_done && api_update_state() == 5) {
                auto_done = 1;
                mlog("TEST update done -> EBOOT.NEW");
            }
        }

        /* 播放 / 下载期间只续「挂起」计时器：屏幕可以照常自动关，但系统不会被
         * 自动挂起（自动挂起会断 WiFi，后台下载和流媒体就废了；用户把机器放着
         * 下载 MV 然后离开时尤其重要）。MV 期间 mv_display_tick 已经 TICK_ALL。 */
        {
            int busy = (audio_state() == 2) || dl_stream_active() ||
                       (api_mv_download_state() == 1);

            if (!busy) {
                int dn = dl_queue_count();

                for (int di = 0; di < dn; ++di) {
                    const DlItem *ditem = dl_queue_at(di);

                    if (ditem && ditem->state == DL_ACTIVE) { busy = 1; break; }
                }
            }
            if (busy) scePowerTick(PSP_POWER_TICK_SUSPEND);
        }

        if ((g_frame % 120) == 0) {
            /* MV 期间也刷：以前为省 FAT 时间跳过 SC_MV，结果一卡死，最后
             * 几十行日志全留在 stdio 缓冲里，最关键的尾巴反而没了
             * （docs/nc0-corruption.md §7）。120 帧 ≈ 2 秒一次，
             * 几毫秒的 FAT 开销远小于丢掉诊断信息。 */
            log_flush();
            sysi_update();
        }

        if ((g_frame % 300) == 0 || (dev_mode() >= 1 && (g_frame % 120) == 0)) {
            int stalls = 0, maxgap = 0;

            audio_stall_stats(&stalls, &maxgap);
            mlog("tick frame=%d screen=%d hasnow=%d st=%d pos=%dms paused=%d stall=%d maxgap=%d "
                 "mem=%dKB/%dKB ms0=%dMB",
                 g_frame, g_app.screen, g_app.has_now, audio_state(), audio_pos_ms(),
                 g_app.paused, stalls, maxgap, sysi_used_kb(), sysi_free_kb(),
                 sysi_ms0_free_mb());
        }

        /* A live/finished decoder owns input and presentation until safe join. */
        if ((g_mv_ui_retained || g_mv_connecting || mv_active() || mv_finished()) && g_app.screen != SC_MV)
            set_screen(SC_MV);

        if (g_exit_req) {
            if (g_mv_connecting) {
                mv_connect_cancel();
            } else if (g_app.screen == SC_MV) {
                int i;

                mv_stop();
                for (i = 0; i < 200 && !mv_finished(); ++i) {
                    sceKernelDelayThread(10 * 1000);
                }
                if (mv_finished()) mv_join();
            }
            mlog("exit requested by system");
            running = 0;
        }

        if(!notice_requested&&g_app.online&&g_app.screen!=SC_MV){notice_requested=1;api_request_announcement_check();}
        if(!notice_seen&&community_announcement_get()&&g_app.screen!=SC_MV&&!g_app.upd_dlg){notice_seen=1;notice_open=1;notice_scroll=0;notice_ignore=COMMUNITY_ANNOUNCEMENT_DEFAULT_IGNORE;}
        if(policy_open){
            if(btn_repeat(pressed,held,dt,PSP_CTRL_UP)&&policy_scroll>0)--policy_scroll;
            if(btn_repeat(pressed,held,dt,PSP_CTRL_DOWN)&&policy_scroll<40)++policy_scroll;
            if(pressed&(PSP_CTRL_CROSS|PSP_CTRL_CIRCLE))policy_open=0;
            g_prev_btn=held;
        } else if(notice_open){
            if(btn_repeat(pressed,held,dt,PSP_CTRL_UP)&&notice_scroll>0)--notice_scroll;
            if(btn_repeat(pressed,held,dt,PSP_CTRL_DOWN)&&notice_scroll<100)++notice_scroll;
            if(pressed&PSP_CTRL_SQUARE)notice_ignore=!notice_ignore;
            if(pressed&(PSP_CTRL_CIRCLE|PSP_CTRL_CROSS)){api_request_announcement_dismiss(notice_ignore);notice_open=0;}
            g_prev_btn=held;
        } else if (g_app.upd_dlg) {
            input_upd_dialog(pressed);
            g_prev_btn = held;
        } else if (g_app.confirm_clear) {
            if (pressed & PSP_CTRL_CIRCLE) {
                int what = g_app.confirm_clear;

                g_app.confirm_clear = 0;
                if (what == 1) {
                    /* 正在播缓存里的歌：先停掉，不然解码线程会读到被删的文件 */
                    if (g_app.has_now && g_app.now.id > 0 && cache_has(g_app.now.id)) {
                        audio_stop();
                        g_app.has_now = 0;
                        g_app.paused = 1;
                        g_app.playing_index = -1;
                    }
                    cache_clear();
                    ui_toast("缓存已清空");
                    g_app.cache_sel = 0;
                } else if (what == 2) {
                    api_clear_pending_downloads();dl_queue_clear();
                    ui_toast("下载队列已清空");
                    g_app.dl_sel = 0;
                } else if (what == 3) {
                    api_logout();
                    ui_toast("已退出登录");
                } else if (what == 4) {
                    api_request_export(&g_export_song);
                    ui_toast("正在覆盖导出");
                }
            } else if (pressed & PSP_CTRL_CROSS) {
                g_app.confirm_clear = 0;
                ui_toast("已取消");
            }
            g_prev_btn = held;
            /* 仍然要画界面/翻页，不能 continue（以前会整屏冻结） */
        } else if (g_exit_confirm) {
            if (pressed & PSP_CTRL_CIRCLE) { mlog("exit confirmed"); running = 0; continue; }
            if (pressed & PSP_CTRL_CROSS) g_exit_confirm = 0;
        }else if(g_app.screen!=SC_MV&&(held&PSP_CTRL_SELECT)&&(pressed&PSP_CTRL_LTRIGGER)){
            open_skin_center();ui_toast("皮肤恢复入口：可选择系统默认");
        } else if (pressed & PSP_CTRL_START) {
            if (g_app.screen != SC_MV) {
                mlog("START pressed");
                g_exit_confirm = 1;
            }
        } else if (!g_exit_req) {
            if(update_gallery_active()){update_gallery_input(pressed);pressed=held=0;}
            if(skin_options_dialog_active()){skin_options_dialog_input(pressed,held,dt);pressed=held=0;}
            if(g_app.has_now&&(pressed&PSP_CTRL_TRIANGLE)&&(g_app.screen==SC_HOME||g_app.screen==SC_MINE)){
                set_screen(SC_PLAYER);pressed&=~PSP_CTRL_TRIANGLE;
            }
            if(skin_transition_active()){
                if(pressed&PSP_CTRL_CROSS){skin_transition_cancel();g_app.cards_open=0;g_app.play_all_next=0;}
                pressed=0; /* keep sampled/held state: no manufactured key edges */
            }
            if(!skin_transition_active()&&g_app.screen!=SC_SKINS&&g_app.screen!=SC_MV&&skin_runtime_input(g_app.screen,pressed,held,dt))pressed=held=0;
            if(!skin_transition_active())switch (g_app.screen) {
            case SC_BOOT:     input_boot(pressed); break;
            case SC_WIFI:     input_wifi(pressed, held, dt); break;
            case SC_HOME:     input_home(pressed, held, dt); break;
            case SC_LIST:     input_list(pressed, held, dt); break;
            case SC_PLAYER:   input_player(pressed, held, dt); break;
            case SC_LYRICS:   input_lyrics(pressed, held, dt); break;
            case SC_SEARCH:   input_search(pressed, held, dt); break;
            case SC_DOWNLOAD: input_download(pressed, held, dt); break;
            case SC_CACHE:    input_cache(pressed, held, dt); break;
            case SC_SETTINGS: input_settings(pressed, held, dt); break;
            case SC_LOGIN:    input_login(pressed, held, dt); break;
            case SC_GRID:     input_grid(pressed, held, dt); break;
            case SC_SQUARE:   input_square(pressed, held, dt); break;
            case SC_COMMENTS: input_comments(pressed, held, dt); break;
            case SC_MINE:     input_mine(pressed, held, dt); break;
            case SC_ALT:      input_alt(pressed, held, dt); break;
            case SC_SKINS:input_skins(pressed,held,dt);break;
            case SC_MV:
                if (pressed & PSP_CTRL_CROSS) {
                    if (g_mv_connecting) mv_connect_cancel();
                    else mv_stop();
                }
                break;
            case SC_HELP:
                if(btn_repeat(pressed,held,dt,PSP_CTRL_UP)&&g_app.help_page>0)--g_app.help_page;
                if(btn_repeat(pressed,held,dt,PSP_CTRL_DOWN))++g_app.help_page;
                if(pressed&PSP_CTRL_LTRIGGER){g_app.help_page-=8;if(g_app.help_page<0)g_app.help_page=0;}
                if(pressed&PSP_CTRL_RTRIGGER)g_app.help_page+=8;
                if ((pressed & PSP_CTRL_CIRCLE) || (pressed & PSP_CTRL_CROSS))
                    set_screen(SC_SETTINGS);
                break;
            default: break;
            }
        }

        if(g_app.screen!=SC_MV&&!g_exit_req)skin_actions_tick();
        /* 每帧检查一次续页：光标靠近列表底部就提前把下一页要回来，
         * 等用户真的滚到底时数据已经在了。 */
        if (!g_exit_req && (g_app.screen == SC_LIST ||
                            (g_app.screen == SC_SEARCH && !g_app.kb_edit&&!g_app.search_kind)))
            maybe_load_more();

        if (!g_exit_req) mv_pending_tick();

        if ((g_mv_ui_retained || g_mv_connecting || mv_active() || mv_finished()) && g_app.screen != SC_MV)
            set_screen(SC_MV);

        if (g_app.screen == SC_MV && g_mv_connecting) {
            if(!g_mv_connect_drawn)mv_connect_step("before-first-connecting-draw");
            long long total=mv_stream_active()?mv_stream_total():api_mv_download_total();
            long long available=mv_stream_active()?mv_stream_available():total*api_mv_download_pct()/100;
            ui_mv_connecting(g_app.now.name,available,total);
            if(!g_mv_connect_drawn)mv_connect_step("after-first-connecting-draw");
            gfx_flip();
            if(!g_mv_connect_drawn){mv_connect_step("after-first-connecting-flip");g_mv_connect_drawn=1;}
        } else if (g_app.screen == SC_MV) {
            if (!mv_display_tick()) sceKernelDelayThread(2000);
            if (mv_finished()) {
                if (mv_join() == 0) {
                    if (mv_result() == -0x4d5602) ui_toast("原生硬解暂不支持此片源格式");
                    else if(mv_result()<0)ui_toast("MV读取或解码失败：请降低清晰度或下载后播放");
                    if (audio_state() == AUDIO_ERROR) ui_toast(audio_error());
                    else {
                        /* Restoring MP3 uses the normal seek/buffer transition.
                         * Give it the same grace as a user seek before cache recovery. */
                        g_seek_guard_t = g_app.time;
                        if(!mv_active()){skin_runtime_resume();ui_theme_changed();}
    audio_pause(g_mv_restore_pause);
                    }
                    skin_runtime_resume();ui_theme_changed();
                    set_screen(g_mv_from);
                } else {
                    g_mv_ui_retained = 1;
                    mlog("mv: live worker retained, restart required; keep video screen");
                }
            }
        } else {
            if (g_exit_confirm) g_app.confirm_clear = 5;
            g_app.time=now_sec(); /* Catalog I/O must not age the first closing frame. */
            ui_draw();
            if(policy_open)ui_document("隐私政策",community_privacy_text(),policy_scroll,"○ / × 返回设置");
            else if(notice_open){const CommunityAnnouncement*n=community_announcement_get();if(n)ui_document(n->title,n->text,notice_scroll,notice_ignore?"[已选] 下次不弹  □切换  ○关闭":"[未选] 下次不弹  □切换  ○关闭");}
            if (g_exit_confirm) g_app.confirm_clear = 0;
            if(update_gallery_active()){ui_free_skin_preview();update_gallery_draw(g_app.time);}
            gfx_flip();
        }

        /* 绘制期间（可能有几十毫秒）按下的键：再采一次攒给下一帧，
           否则 100ms 内的一按一松会被整帧丢掉 —— 就是「要点两遍」的根源 */
        {
            SceCtrlData pad2;

            if (sceCtrlPeekBufferPositive(&pad2, 1) > 0)
                g_pending_press |= (pad2.Buttons & ~sampled_held);
        }

        if (g_frame < 4) mlog("frame %d drawn", g_frame);
        if (g_frame == 4) {
            sysi_update();
            mlog("boot: local scan");
            local_scan();
            mlog("boot: drop partial");
            /* Valid .part checkpoints belong to the resumable downloader. */
            mlog("boot: adopt orphans");
            adopt_cache_orphans();
            mlog("boot: prune");
            cache_prune();          /* 认领完了再清理残片 */
            mlog("boot: mv index");
            mv_index_init();        /* 没有索引时扫一次 cache，把已存在的 MV 补进列表 */
            mlog("boot: cache ready");
            mlog("sys: mem=%dKB free=%dKB maxblk=%dKB ms0=%dMB/%dMB", sysi_used_kb(),
                 sysi_free_kb(), sysi_maxblock_kb(), sysi_ms0_free_mb(), sysi_ms0_total_mb());
        }

        if (g_app.screen != last_screen) {
            static const char *tag[SC_COUNT] = {
                "boot", "wifi", "home", "list", "player", "lyrics",
                "search", "download", "cache", "settings", "help", "login",
                "grid", "square", "comments", "mine", "mv", "alt", "skins"
            };
            last_screen = g_app.screen;
            mlog("screen -> %s (t=%.1f)", tag[g_app.screen], g_app.time);
            if (dev_mode() != 3) shot(tag[g_app.screen]);
        }
        g_frame++;
    }

    /* 退出前先把刚下载的更新换上：下次启动直接是新版本，不用再进两次。 */
    update_apply_startup();

    mlog("exit: stopping audio");
    audio_stop();
    audio_shutdown();

    /* 先把该写的写完，再停线程：万一某个线程在文件锁里被强杀，
     * 后面的 store_save/log_close 会永远等锁（以前只能拔电池）。 */
    mlog("exit: save store");
    store_save();
    log_flush();

    forced = 0;
    mlog("exit: stopping dl");
    forced = dl_shutdown();
    if (!forced) {
        mlog("exit: stopping api");
        forced = api_shutdown();
    update_gallery_shutdown();
    }
    if (!forced) {
        mlog("exit: net term");
        net_term();
        mlog("exit: bye");
        log_close();
    }

    sceKernelDelayThread(100 * 1000);
    sceKernelExitGame();
    sceKernelExitDeleteThread(0);
    return 0;
}
