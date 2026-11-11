/*
 * MELODY PSP — 界面绘制
 * ------------------------------------------------------------------
 * 设计：网易云音乐网页版（深色顶栏 #242424 + 内容区 + 品牌红 #C20C0C）。
 * 字号：12px 元信息 / 16px 正文 / 22px 标题 / 32px 大数字。
 *
 * 纵向分区（480x272）：
 *   0..28      顶栏（黑色，logo + 发现/我的/搜索 或 返回 + 页面标题）
 *   28..218    内容区（播放中时到 218，否则到 258）
 *   218..231   按键提示条（黑色）
 *   232..272   迷你播放器（黑色；没在播放时不占位）
 *
 * 所有界面只读状态，不改状态；输入逻辑在 main.c。
 */

#include "ui.h"
#include "power_runtime.h"
#include "ime_update.h"
#include "overclock_dialog.h"
#include "overclock_bridge.h"
#include "ime.h"
#include "search_history.h"

#include "api.h"
#include "lyric_motion.h"
#include "lyric_render.h"
#include "comment_view.h"
#include "alt.h"
#include "audio.h"
#include "cache.h"
#include "dl.h"
#include "dl_progress_contract.h"
#include "gfx.h"
#include "local.h"
#include "log.h"
#include "mv.h"
#include "mvexport.h"
#include "settings_order.h"
#include "update_log_view.h"
#include "community.h"
#include "credits.h"
#include "net.h"
#include "store.h"
#include "sysinfo.h"
#include "skin_runtime.h"
#include "skin_options_ui.h"
#include "skin_transition.h"
#include "skin_disc.h"
#include "skin_catalog.h"
#include "png.h"

#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRAND_RED() (skin_runtime_active()?C_ACC:gfx_rgb(230, 0, 38))       /* logo 圆盘 #E60026 */
#define NAV_BG()    gfx_rgb(0x24, 0x24, 0x24) /* 顶栏 #242424 */
#define PLAYER_BG() gfx_rgb(0x22, 0x22, 0x22)

/* ------------------------------------------------------------------ */
/* 主题                                                                */
/* ------------------------------------------------------------------ */

static unsigned short C_BG, C_PANEL, C_PANEL2, C_LINE, C_TXT, C_SUB, C_MUTE;
static unsigned short C_ACC, C_HOT, C_LINK, C_OK, C_WARN, C_ROW;
static unsigned short C_NAV, C_NAV_TXT, C_NAV_DIM, C_PLAYER, C_PTXT, C_PSUB;
static unsigned short C_WHITE, C_BLACK, C_SHADOW;

static int g_dark;

static int is_metro_style(void)
{
    const char*id=skin_runtime_style()->id;
    return skin_runtime_active() && (!strcmp(id,"metro")||!strcmp(id,"metro_day"));
}

static unsigned short metro_background(void)
{
    return C_BG;
}

/* Native dialogs remain above Lua pages, using the active Metro palette. */
static void modal_fill(int x,int y,int w,int h,int radius,unsigned short color)
{
    if(is_metro_style())gfx_fill_rect(x,y,w,h,color == C_PANEL ? metro_background() : color);
    else gfx_fill_round(x,y,w,h,radius,color);
}
static void modal_fill_a(int x,int y,int w,int h,int radius,unsigned short color,int alpha)
{
    if(is_metro_style()){
        /* Metro surfaces have no drop shadow; fading notices retain opacity. */
        if(color==C_BLACK && alpha<=60)return;
        gfx_fill_rect_a(x,y,w,h,color,alpha);
    }else gfx_fill_round_a(x,y,w,h,radius,color,alpha);
}
static void modal_border(int x,int y,int w,int h,int radius,unsigned short color)
{
    if(is_metro_style()){
        gfx_fill_rect(x,y,w,2,C_ACC);
        gfx_fill_rect(x,y+h-1,w,1,color);
        gfx_fill_rect(x,y,1,h,color);gfx_fill_rect(x+w-1,y,1,h,color);
    }else gfx_round_border(x,y,w,h,radius,color);
}

static void theme_apply(int dark)
{
    if(skin_runtime_active())dark=skin_runtime_style()->dark;
    g_dark = dark ? 1 : 0;

    C_WHITE = gfx_rgb(255, 255, 255);
    C_BLACK = gfx_rgb(0, 0, 0);

    C_NAV = NAV_BG();
    C_PLAYER = PLAYER_BG();
    C_NAV_TXT = gfx_rgb(0xEC, 0xEC, 0xEC);
    C_NAV_DIM = gfx_rgb(0xA8, 0xA8, 0xA8);
    C_PTXT = gfx_rgb(0xE8, 0xE8, 0xE8);
    C_PSUB = gfx_rgb(0x9B, 0x9B, 0x9B);

    if (!g_dark) {
        C_BG = gfx_rgb(0xEF, 0xEF, 0xEF);
        C_PANEL = gfx_rgb(0xFB, 0xFB, 0xFB);
        C_PANEL2 = gfx_rgb(0xF1, 0xF1, 0xF1);
        C_LINE = gfx_rgb(0xE3, 0xE3, 0xE3);
        C_TXT = gfx_rgb(0x22, 0x22, 0x22);
        C_SUB = gfx_rgb(0x66, 0x66, 0x66);
        C_MUTE = gfx_rgb(0x9A, 0x9A, 0x9A);
        C_ACC = gfx_rgb(0xC2, 0x0C, 0x0C);
        C_HOT = gfx_rgb(0xEF, 0x27, 0x27);
        C_LINK = gfx_rgb(0x0C, 0x73, 0xC2);
        C_OK = gfx_rgb(0x2B, 0xA4, 0x71);
        C_WARN = gfx_rgb(0xE0, 0x4B, 0x4B);
        C_ROW = gfx_rgb(0xEC, 0xEC, 0xEC);
        C_SHADOW = gfx_rgb(0x9A, 0x9A, 0x9A);
    } else {
        C_BG = gfx_rgb(0x10, 0x10, 0x12);
        C_PANEL = gfx_rgb(0x18, 0x18, 0x1A);
        C_PANEL2 = gfx_rgb(0x24, 0x24, 0x27);
        C_LINE = gfx_rgb(0x2D, 0x2D, 0x31);
        C_TXT = gfx_rgb(0xEA, 0xEA, 0xEA);
        C_SUB = gfx_rgb(0xA2, 0xA2, 0xA8);
        C_MUTE = gfx_rgb(0x70, 0x70, 0x78);
        C_ACC = gfx_rgb(0xEC, 0x41, 0x41);
        C_HOT = gfx_rgb(0xFF, 0x5F, 0x5F);
        C_LINK = gfx_rgb(0x4A, 0x9E, 0xE0);
        C_OK = gfx_rgb(0x3E, 0xCF, 0x8E);
        C_WARN = gfx_rgb(0xFF, 0x6B, 0x6B);
        C_ROW = gfx_rgb(0x26, 0x26, 0x2A);
        C_SHADOW = gfx_rgb(0x00, 0x00, 0x00);
    }
    const SkinStyle*style=skin_runtime_style();unsigned short*dest[]={&C_BG,&C_PANEL,&C_TXT,&C_SUB,&C_MUTE,&C_LINE,&C_ACC,&C_HOT,&C_PANEL2,&C_WHITE,&C_BLACK,&C_OK,&C_WARN,&C_ROW,&C_SHADOW};
    if(skin_runtime_active())for(int i=0;i<SKIN_COLOR_COUNT;++i)if(style->color_mask&(1u<<i))*dest[i]=style->colors[i];
    if(skin_runtime_active())C_LINK=C_ACC;
    if(is_metro_style()){
        C_NAV=C_PLAYER=C_BG;
        C_NAV_TXT=C_PTXT=C_TXT;
        C_NAV_DIM=C_PSUB=C_SUB;
    }

}

/* ------------------------------------------------------------------ */
/* 缓存视图（避免每帧抢信号量）                                        */
/* ------------------------------------------------------------------ */

static CacheEntry g_cache_view[CACHE_MAX];
static int g_cache_view_n;
static int g_cache_view_gen = -1;
static int g_cache_view_first=-1,g_cache_total;

static DlItem g_dl_view[DL_QUEUE_MAX];
static int g_dl_view_n;
static int g_dl_view_gen = -1;

static void views_refresh(void)
{
    int gen = cache_gen();

    int selected=g_app.cache_sel,total=cache_count();
    int first=g_cache_view_first;
    int reload=gen!=g_cache_view_gen||first<0||selected<first||selected>=first+g_cache_view_n;
    if(!reload&&g_cache_view_n==CACHE_MAX){
        if(selected<first+8&&first>0)reload=1;
        if(selected>=first+g_cache_view_n-8&&first+g_cache_view_n<total)reload=1;
    }
    if(reload){first=selected>16?selected-16:0;}
    if (reload) {
        g_cache_view_gen = gen;g_cache_view_first=first;g_cache_total=cache_count();
        g_cache_view_n = cache_snapshot_page(g_cache_view, CACHE_MAX,first);
    }

    static unsigned last_download_refresh;
    unsigned now=sceKernelGetSystemTimeLow();
    gen = dl_queue_gen();
    if(gen!=g_dl_view_gen||now-last_download_refresh>=200000u){
        last_download_refresh=now;
        g_dl_view_gen = gen;
        g_dl_view_n = dl_queue_snapshot(g_dl_view,DL_QUEUE_MAX);
    }
}

static int cache_view_has(long long id)
{
    return cache_has(id);
}

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static float clamp01f(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static int clamp_i(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

static float approach(float cur, float target, float speed, float dt)
{
    float d = target - cur;
    float step = speed * dt;

    if (d > step) d = step;
    if (d < -step) d = -step;
    return cur + d;
}

static void fmt_time(char *buf, int len, int ms)
{
    if (ms < 0) ms = 0;
    snprintf(buf, (size_t)len, "%d:%02d", ms / 60000, (ms / 1000) % 60);
}

static void fmt_size(char *buf, int len, long long bytes)
{
    if (bytes >= 1024 * 1024)
        snprintf(buf, (size_t)len, "%.1fMB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(buf, (size_t)len, "%lldKB", bytes / 1024);
    else
        snprintf(buf, (size_t)len, "%lldB", bytes);
}

static void fmt_count(char *buf, int size, long long n)
{
    if (n >= 100000000) snprintf(buf, size, "%.1f亿", (double)n / 100000000.0);
    else if (n >= 10000) snprintf(buf, size, "%.1f万", (double)n / 10000.0);
    else if (n > 0) snprintf(buf, size, "%lld", n);
    else buf[0] = 0;
}

/* "已下载/总数" 用同一个百分比反算，保证数字和进度条不打架。 */
static void fmt_done_total(char *buf, int len, long long done, long long total)
{
    if (total <= 0) { fmt_size(buf, len, done); return; }
    if (total >= 1024 * 1024)
        snprintf(buf, (size_t)len, "%.1f/%.1fMB", (double)done / (1024.0 * 1024.0),
                 (double)total / (1024.0 * 1024.0));
    else
        snprintf(buf, (size_t)len, "%lld/%lldKB", done / 1024, total / 1024);
}

/* 取 UTF-8 的第一个字符（头像没有图时用来画首字，不能按字节截） */
static void first_char(const char *s, char *out, int n)
{
    unsigned char c;
    int len = 1;

    out[0] = 0;
    if (!s || !s[0] || n < 2) return;
    c = (unsigned char)s[0];
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (len > n - 1) len = n - 1;
    memcpy(out, s, (size_t)len);
    out[len] = 0;
}

/* 内容区 / 提示条 / 播放器 的纵向分界 */
static int mini_bar(void)
{
    if (!g_app.has_now) return 0;
    if (g_app.screen == SC_PLAYER || g_app.screen == SC_LYRICS) return 0;
    return 1;
}

static int hint_y(void) { return SCR_H - UI_HINT_H - (mini_bar() ? UI_PLAYER_H : 0); }
static int content_bottom(void) { return hint_y(); }
static int player_y(void) { return SCR_H - UI_PLAYER_H; }

static void panel_bg(void)
{
    gfx_fill_rect(0, UI_CONTENT_TOP, SCR_W, SCR_H, C_PANEL);
}

/* 选中态：整行底色 + 左侧 2px 红条 */
static void row_focus(int x, int y, int w, int h)
{
    gfx_fill_round(x, y, w, h, 3, C_ROW);
    gfx_fill_round(x + 4, y + (h - 14) / 2, 2, 14, 1, C_ACC);
}

/* 列表右侧滚动条：total/view 是行数，offset 是首行 */
static void scrollbar_draw(int y0, int h, int total, int view, int offset)
{
    int bh, maxoff, by;

    if (total <= view || view <= 0 || h <= 4) return;

    bh = h * view / total;
    if (bh < 14) bh = 14;
    if (bh > h) bh = h;
    maxoff = total - view;
    by = y0 + (maxoff > 0 ? (int)((long long)(h-bh)*offset/maxoff) : 0);
    if (by < y0) by = y0;
    if (by + bh > y0 + h) by = y0 + h - bh;

    gfx_fill_round(SCR_W - 4, y0 + 1, 2, h - 2, 1, C_PANEL2);
    gfx_fill_round(SCR_W - 4, by, 2, bh, 1, C_ACC);
}

/* ------------------------------------------------------------------ */
/* 图标（全部矢量，1~2px 笔画）                                        */
/* ------------------------------------------------------------------ */

static void icon_play(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_tri(cx - s / 2 + 1, cy - s, cx - s / 2 + 1, cy + s, cx + s, cy, c, a);
}

static void icon_pause(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_fill_round_a(cx - s / 2 - 1, cy - s, 3, s * 2, 1, c, a);
    gfx_fill_round_a(cx + s / 2 - 2, cy - s, 3, s * 2, 1, c, a);
}

static void icon_prev(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_fill_round_a(cx - s - 1, cy - s, 2, s * 2, 1, c, a);
    gfx_tri(cx + s, cy - s, cx + s, cy + s, cx - s + 1, cy, c, a);
}

static void icon_next(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_fill_round_a(cx + s, cy - s, 2, s * 2, 1, c, a);
    gfx_tri(cx - s, cy - s, cx - s, cy + s, cx + s - 1, cy, c, a);
}

static void icon_search(int cx, int cy, int r, unsigned short c, int a)
{
    gfx_ring_a(cx, cy, r, r - 2, c, a);
    for (int i = 0; i < r - 1; ++i)
        gfx_fill_rect_a(cx + r - 2 + i, cy + r - 2 + i, 2, 2, c, a);
}

static void icon_download(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_fill_round_a(cx - 1, cy - s, 2, s + 1, 1, c, a);
    gfx_tri(cx - 3, cy, cx + 3, cy, cx, cy + 4, c, a);
    gfx_fill_round_a(cx - s, cy + s - 1, s * 2, 2, 1, c, a);
}

static void icon_disk(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_round_border_a(cx - s, cy - s, s * 2, s * 2, 3, c, a);
    gfx_ring_a(cx, cy, s / 3 + 1, s / 3, c, a);
}

static void icon_chevron_left(int x, int y, unsigned short c, int a)
{
    for (int i = 0; i < 5; ++i) {
        gfx_fill_rect_a(x + i, y + 5 - i, 2, 2, c, a);
        gfx_fill_rect_a(x + i, y + 5 + i, 2, 2, c, a);
    }
}

static void icon_chevron_right(int x, int y, unsigned short c, int a)
{
    for (int i = 0; i < 5; ++i) {
        gfx_fill_rect_a(x + 4 - i, y + 5 - i, 2, 2, c, a);
        gfx_fill_rect_a(x + 4 - i, y + 5 + i, 2, 2, c, a);
    }
}

static void icon_headset(int cx, int cy, unsigned short c, int a)
{
    gfx_arc(cx, cy + 1, 5, 4, 180, 360, c, a);
    gfx_fill_rect_a(cx - 5, cy + 1, 2, 4, c, a);
    gfx_fill_rect_a(cx + 3, cy + 1, 2, 4, c, a);
}

static void icon_note(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_circle_a(cx - s / 2, cy + s / 2, s / 3, c, a);
    gfx_circle_a(cx + s / 2, cy + s / 2 - s / 4, s / 3, c, a);
    gfx_fill_round_a(cx - s / 2 + s / 3 - 1, cy - s / 2, 2, s, 1, c, a);
    gfx_fill_round_a(cx + s / 2 + s / 3 - 1, cy - s / 2 - s / 4, 2, s, 1, c, a);
    gfx_fill_rect_a(cx - s / 2 + s / 3, cy - s / 2, s / 2 + 2, 2, c, a);
}

static void icon_heart(int cx, int cy, int s, unsigned short c, int a)
{
    int r = (s * 3) / 7;

    if (r < 2) r = 2;
    gfx_circle_a(cx - r, cy - r / 2, r, c, a);
    gfx_circle_a(cx + r, cy - r / 2, r, c, a);
    gfx_tri(cx - r * 2 + 1, cy, cx + r * 2 - 1, cy, cx, cy + r * 2 + 1, c, a);
}

static void icon_comment(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_fill_round_a(cx - s, cy - s + 1, s * 2, s * 2 - 4, 3, c, a);
    gfx_tri(cx - s / 3, cy + s - 3, cx + s / 3, cy + s - 3, cx - s / 2, cy + s + 2, c, a);
    gfx_fill_rect_a(cx - s + 2, cy - s + 3, s * 2 - 4, 1, C_PANEL, a);
}

static void icon_list(int cx, int cy, int s, unsigned short c, int a)
{
    for (int i = -1; i <= 1; ++i)
        gfx_fill_round_a(cx - s, cy + i * 4 - 1, s * 2, 2, 1, c, a);
}

static void icon_person(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_circle_a(cx, cy - s / 3, s / 2, c, a);
    gfx_fill_round_a(cx - s, cy + s / 4, s * 2, s, s / 2, c, a);
}

static void icon_gear(int cx, int cy, int s, unsigned short c, int a)
{
    gfx_ring_a(cx, cy, s, s - 3, c, a);
    gfx_circle_a(cx, cy, 2, c, a);
    gfx_fill_rect_a(cx - s - 2, cy - 1, 3, 2, c, a);
    gfx_fill_rect_a(cx + s, cy - 1, 3, 2, c, a);
}

static void icon_volume(int cx, int cy, unsigned short c, int a)
{
    gfx_fill_round_a(cx - 6, cy - 2, 3, 5, 1, c, a);
    gfx_tri(cx - 3, cy - 5, cx - 3, cy + 5, cx + 2, cy, c, a);
    gfx_arc(cx + 3, cy, 5, 4, 300, 60, c, a);
}

static void icon_check(int cx, int cy, unsigned short c, int a)
{
    for (int i = 0; i < 3; ++i)
        gfx_fill_rect_a(cx - 4 + i, cy - 1 + i, 2, 2, c, a);
    for (int i = 0; i < 5; ++i)
        gfx_fill_rect_a(cx - 1 + i, cy + 1 - i, 2, 2, c, a);
}

static void icon_wifi(int x, int y, int on)
{
    unsigned short c = on ? C_NAV_TXT : C_NAV_DIM;

    for (int i = 0; i < 3; ++i) {
        int r = 3 + i * 3;
        gfx_arc(x, y, r, r - 2, 215, 325, c, on ? 220 : 90);
    }
    gfx_fill_rect(x - 1, y - 1, 2, 2, c);
}

static void icon_battery(int x, int y, int pct)
{
    int w = 18, h = 8;

    gfx_round_border_a(x, y, w, h, 2, C_NAV_DIM, 200);
    gfx_fill_round_a(x + w + 1, y + 2, 2, 3, 1, C_NAV_DIM, 200);
    if (pct > 0) {
        int fw = (w - 4) * pct / 100;
        if (fw < 1) fw = 1;
        gfx_fill_round(x + 2, y + 2, fw, h - 4, 1, pct > 25 ? C_NAV_TXT : C_WARN);
    }
}

/* 播放模式图标：0 顺序 1 随机 2 单曲 */
static void icon_mode(int cx, int cy, int mode, unsigned short c, int a)
{
    if (mode == 1) {
        gfx_arc(cx, cy, 5, 4, 20, 160, c, a);
        gfx_arc(cx, cy, 5, 4, 200, 340, c, a);
        gfx_fill_rect_a(cx + 3, cy - 5, 3, 2, c, a);
        gfx_fill_rect_a(cx - 5, cy + 4, 3, 2, c, a);
    } else if (mode == 2) {
        gfx_arc(cx, cy, 5, 4, 0, 350, c, a);
        gfx_fill_rect_a(cx - 1, cy + 1, 2, 2, c, a);
    } else {
        gfx_arc(cx, cy, 5, 4, 20, 160, c, a);
        gfx_arc(cx, cy, 5, 4, 200, 340, c, a);
        gfx_fill_rect_a(cx + 3, cy - 5, 3, 2, c, a);
        gfx_fill_rect_a(cx + 3, cy - 5, 2, 3, c, a);
    }
}

/* 正在播放的三根跳动音柱 */
int ui_skin_icon(const char*name,int x,int y,int size,unsigned short color){
 int cx=x+size/2,cy=y+size/2,s=size/2;if(s<2)s=2;
 if(!strcmp(name,"music"))icon_note(cx,cy,s,color,255);
 else if(!strcmp(name,"play"))icon_play(cx,cy,s,color,255);
 else if(!strcmp(name,"pause"))icon_pause(cx,cy,s,color,255);
 else if(!strcmp(name,"previous"))icon_prev(cx,cy,s,color,255);
 else if(!strcmp(name,"next"))icon_next(cx,cy,s,color,255);
 else if(!strcmp(name,"heart"))icon_heart(cx,cy,s,color,255);
 else if(!strcmp(name,"comment"))icon_comment(cx,cy,s,color,255);
 else if(!strcmp(name,"lyrics"))icon_list(cx,cy,s,color,255);
 else if(!strcmp(name,"back"))icon_chevron_left(cx-3,cy,color,255);
 else if(!strcmp(name,"download"))icon_download(cx,cy,s,color,255);
 else if(!strcmp(name,"gear"))icon_gear(cx,cy,s,color,255);
 else if(!strcmp(name,"search"))icon_search(cx,cy,s,color,255);
 else if(!strcmp(name,"wifi"))icon_wifi(x,y,1);
 else if(!strcmp(name,"star")){for(int i=0;i<10;i++){float a=(i*36-90)*.01745329252f,b=((i+1)*36-90)*.01745329252f;int ra=i%2?s/2:s,rb=(i+1)%2?s/2:s;gfx_tri(cx,cy,cx+(int)(cosf(a)*ra),cy+(int)(sinf(a)*ra),cx+(int)(cosf(b)*rb),cy+(int)(sinf(b)*rb),color,255);}}
 else return 0;return 1;
}

static void icon_bars(int cx, int cy, int h, float phase, unsigned short c)
{
    for (int i = 0; i < 3; ++i) {
        int hh = h / 3 + (int)(h * 0.34f * (0.5f + 0.5f * sinf(phase * 4.0f + i * 1.7f)));
        gfx_fill_round(cx + i * 5 - 5, cy - hh / 2, 3, hh, 1, c);
    }
}

/* ------------------------------------------------------------------ */
/* 组件                                                                */
/* ------------------------------------------------------------------ */

/* 封面：没有图时画渐变底 + 音符（比空白好看，和网易云默认封面同款思路） */
static void draw_cover(int x, int y, int s, int r, const unsigned short *px,
                       int sw, int sh)
{
    if (!g_dark)
        gfx_fill_round_a(x + 1, y + 2, s, s, r, C_BLACK, 22);
    else
        gfx_fill_round_a(x + 1, y + 2, s, s, r, C_BLACK, 70);

    if (px && sw > 0 && sh > 0) {
        gfx_blit565_round(x, y, s, s, px, sw, sh, r, 255);
    } else {
        gfx_fill_round_grad(x, y, s, s, r, gfx_lerp565(C_PANEL2, C_ACC, 12),
                            gfx_lerp565(C_PANEL2, C_ACC, 34));
        gfx_ring_a(x + s / 2, y + s / 2, s / 4, s / 4 - 1, C_MUTE, 70);
        icon_note(x + s / 2, y + s / 2 - 1, s / 7, C_MUTE, 200);
    }
}

static const unsigned short *thumb_of(const char *url)
{
    int slot;

    if (!url || !url[0]) return NULL;
    slot = api_thumb_slot(url);
    if (slot < 0) return NULL;
    return api_thumb_px(slot);
}

/* 建议按钮：红色胶囊 */
static void btn_red(int x, int y, int w, int h, const char *label)
{
    gfx_fill_round(x, y, w, h, h / 2, C_ACC);
    gfx_text_center(FS_SM, x, w, y + (h - 12) / 2, label, C_WHITE);
}

/* 小标签：VIP / 热门 / 独家 */
static void tag_chip(int x, int y, const char *label, unsigned short col, int filled)
{
    int w = gfx_text_w(FS_SM, label) + 8;

    if (filled) {
        gfx_fill_round(x, y, w, 14, 2, col);
        gfx_text_center(FS_SM, x, w, y + 1, label, C_WHITE);
    } else {
        gfx_round_border_a(x, y, w, 14, 2, col, 200);
        gfx_text_center(FS_SM, x, w, y + 1, label, col);
    }
}

/* 分节标题：网易云的标志是标题下面那条 1px 红线和右侧「更多 >」 */
static void section_header(int x, int y, int w, const char *title, const char *more)
{
    gfx_text(FS_MD, x, y, title, C_TXT);
    if (more && more[0]) {
        int mw = gfx_text_w(FS_SM, more);

        gfx_text(FS_SM, x + w - mw - 8, y + 3, more, C_MUTE);
        icon_chevron_right(x + w - 7, y + 3, C_MUTE, 200);
    }
    gfx_fill_rect(x, y + 20, w, 1, C_ACC);
}

static void progress_bar(int x, int y, int w, int h, int pct,
                         unsigned short fg, unsigned short bg)
{
    int filled;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    filled = w * pct / 100;

    modal_fill(x, y, w, h, h / 2, bg);
    if (filled > 1) modal_fill(x, y, filled, h, h / 2, fg);
}

static void toast_draw(void)
{
    int w, a, x, y;

    if (g_app.toast_t <= 0) return;

    w = gfx_text_w(FS_SM, g_app.toast) + 26;
    if (w > SCR_W - 16) w = SCR_W - 16;
    a = (int)(225 * clamp01f(g_app.toast_t * 1.6f));
    x = (SCR_W - w) / 2;
    y = hint_y() - 34;

    modal_fill_a(x, y, w, 24, 12, C_BLACK, a);
    gfx_text_ellipsis(FS_SM, x + 13, y + 6, w - 26, g_app.toast, C_WHITE);
}

static void metro_loading_line(float time){
 int y=(skin_runtime_active()&&skin_runtime_style()->hide_mini)?234:hint_y()-3;
 float phase=fmodf(time,1.25f)/1.25f;int start=(int)(phase*(SCR_W+116))-116;
 gfx_fill_rect_a(0,y,SCR_W,2,C_ACC,35);
 int widths[3]={70,28,12},gaps[3]={0,78,114};for(int i=0;i<3;++i){int x=start+gaps[i];int w=widths[i];if(x<0){w+=x;x=0;}if(x+w>SCR_W)w=SCR_W-x;if(w>0)gfx_fill_rect_a(x,y,w,2,C_ACC,235-i*45);}
}
static void spinner_draw(int x, int y, float phase)
{
    if(is_metro_style()){metro_loading_line(g_app.time);return;}
    for (int i = 0; i < 8; ++i) {
        float ang = (float)(i * 45) * 3.14159f / 180.0f + phase * 4.0f;
        int a = 40 + (i * 27) % 215;

        gfx_circle_a(x + (int)(cosf(ang) * 5), y + (int)(sinf(ang) * 5), 1, C_ACC, a);
    }
}

/* ------------------------------------------------------------------ */
/* 顶栏                                                                */
/* ------------------------------------------------------------------ */

static void nav_logo(int cx, int cy)
{
    gfx_circle(cx, cy, 8, BRAND_RED());
    gfx_circle(cx - 3, cy + 3, 2, C_WHITE);
    gfx_circle(cx + 3, cy + 1, 2, C_WHITE);
    gfx_fill_rect(cx - 1, cy - 4, 1, 8, C_WHITE);
    gfx_fill_rect(cx + 5, cy - 6, 1, 8, C_WHITE);
    gfx_fill_rect(cx - 1, cy - 4, 5, 1, C_WHITE);
}

static void nav_common_bg(void)
{
    gfx_fill_rect(0, 0, SCR_W, UI_NAV_H, C_NAV);
    gfx_hline(0, UI_NAV_H - 1, SCR_W, gfx_rgb(0x1A, 0x1A, 0x1A));
}

/* 顶栏：logo + 发现/我的/搜索。tab: 0/1/2 */
typedef struct {
    int percent, present, online, charging;
    char clock[8];
} NavStatus;

/* Values from SDK status queries are tri-state; errors never mean "true". */
static void nav_status_sample(NavStatus *s)
{
    ScePspDateTime dt;
    int pct;
    s->present = scePowerIsBatteryExist();
    s->online = scePowerIsPowerOnline();
    s->charging = scePowerIsBatteryCharging();
    s->percent = -1;
    if (s->present == 1) {
        pct = scePowerGetBatteryLifePercent();
        if (pct >= 0) s->percent = pct > 100 ? 100 : pct;
    }
    memset(&dt, 0, sizeof(dt));
    snprintf(s->clock, sizeof(s->clock), "--:--");
    if (sceRtcGetCurrentClockLocalTime(&dt) >= 0 && dt.hour <= 23 && dt.minute <= 59)
        snprintf(s->clock, sizeof(s->clock), "%02d:%02d", (int)dt.hour, (int)dt.minute);
}

static void nav_status_draw(void)
{
    static NavStatus status;
    static float last;
    static int sampled;
    char value[16];
    if (!sampled || g_app.time < last || g_app.time - last >= 1.0f) {
        nav_status_sample(&status);
        sampled = 1;
        last = g_app.time;
    }
    gfx_text_right(FS_SM, 402, 8, status.clock, C_NAV_DIM);
    icon_battery(432, 10, status.percent > 0 ? status.percent : 0);
    if (status.present == 0) snprintf(value, sizeof(value), "%s", status.online == 1 ? "AC" : "N/A");
    else if (status.percent < 0) snprintf(value, sizeof(value), "--");
    else snprintf(value, sizeof(value), "%d%%", status.percent);
    gfx_text_right(FS_SM, SCR_W - 2, 8, value, C_NAV_DIM);
    if (status.present == 1 && status.charging == 1)
        gfx_text(FS_SM, 424, 8, "+", C_ACC);
    else if (status.present == 1 && status.online == 1)
        gfx_fill_rect(426, 12, 2, 3, C_NAV_TXT);
}

static void nav_tabs(void)
{
    static const char *tabs[3] = { "发现", "我的", "搜索" };
    int x = 104;

    nav_common_bg();
    nav_logo(17, 14);
    gfx_text(FS_SM, 30, 8, "网易云音乐", C_NAV_TXT);

    for (int i = 0; i < 3; ++i) {
        int tw = gfx_text_w(FS_SM, tabs[i]);
        int on = (g_app.tab == i);

        gfx_text(FS_SM, x, 8, tabs[i], on ? C_WHITE : C_NAV_DIM);
        if (on) gfx_fill_round(x + (tw - 12) / 2, UI_NAV_H - 3, 12, 2, 1, C_ACC);
        x += tw + 22;
    }
    g_app.busy_x = x + 8;

    icon_wifi(410, 20, g_app.wifi_state == 0);
    nav_status_draw();
}

/* 内页顶栏：返回 + 标题 + 右侧文字 */
static void nav_page(const char *title, const char *right, int back)
{
    nav_common_bg();
    if (back) icon_chevron_left(12, 9, C_NAV_TXT, 230);
    gfx_text(FS_MD, back ? 26 : 14, 6, title, C_WHITE);
    g_app.busy_x = (back ? 26 : 14) + gfx_text_w(FS_MD, title) + 14;
    if (right && right[0]) gfx_text_right(FS_SM, 268, 8, right, C_NAV_DIM);
}

/* ------------------------------------------------------------------ */
/* 按键提示条                                                          */
/* ------------------------------------------------------------------ */

static int hint_x;
static int hint_used;

static void hint_begin(void)
{
    int y = hint_y();

    hint_x = 10;
    hint_used = 0;
    gfx_fill_rect(0, y, SCR_W, UI_HINT_H, C_NAV);
    gfx_hline(0, y, SCR_W, gfx_rgb(0x1A, 0x1A, 0x1A));
}

static void hint_item(const char *key, const char *label)
{
    int y = hint_y();
    int kw = gfx_text_w(FS_SM, key) + 6;
    int lw = gfx_text_w(FS_SM, label);
    int total = kw + 4 + lw + 10;

    if (hint_used + total > SCR_W - 92) return;

    gfx_fill_round(hint_x, y + 2, kw, 10, 2, gfx_rgb(0x38, 0x38, 0x38));
    gfx_text_center(FS_SM, hint_x, kw, y + 1, key, C_NAV_TXT);
    gfx_text(FS_SM, hint_x + kw + 3, y + 1, label, C_NAV_DIM);
    hint_x += total;
    hint_used += total;
}

static void hint_end(void)
{
    if(g_app.show_mem&&!is_metro_style()){
        char line[48];
        int free_kb = sysi_free_kb();
        int used_kb = sysi_used_kb();
        int total_kb = used_kb + free_kb;

        if (total_kb <= 0) total_kb = 32 * 1024;
        snprintf(line, sizeof(line), "%d.%d/%d.%dM", used_kb / 1024,
                 (used_kb % 1024) * 10 / 1024, total_kb / 1024,
                 (total_kb % 1024) * 10 / 1024);
        gfx_text_right(FS_SM, SCR_W - 6, hint_y() + 1, line, C_MUTE);
    }
}

/* ------------------------------------------------------------------ */
/* 底部迷你播放器                                                      */
/* ------------------------------------------------------------------ */

static void player_bar_draw(void)
{
    const Song *s = &g_app.now;
    int y = player_y();
    int dur = audio_dur_ms() > 0 ? audio_dur_ms() : s->dur_ms;
    int pos = audio_pos_ms();
    int pct = dur > 0 ? (int)((long long)100 * pos / dur) : 0;
    int aw = 0, ah = 0;
    const unsigned short *art = api_art(&aw, &ah);

    if (!mini_bar()) return;

    gfx_fill_rect(0, y, SCR_W, UI_PLAYER_H, C_PLAYER);
    gfx_hline(0, y, SCR_W, gfx_rgb(0x14, 0x14, 0x14));

    if (art && aw > 0)
        gfx_blit565_round(8, y + 7, 26, 26, art, aw, ah, 3, 255);
    else {
        gfx_fill_round(8, y + 7, 26, 26, 3, gfx_rgb(0x36, 0x36, 0x36));
        icon_note(21, y + 19, 4, gfx_rgb(0x88, 0x88, 0x88), 220);
    }

    if (!g_app.paused && audio_state() != AUDIO_ERROR)
        gfx_fill_round(7, y + 12, 2, 16, 1, C_HOT);

    gfx_text_ellipsis(FS_SM, 40, y + 4, 190, s->name[0] ? s->name : "未在播放", C_PTXT);
    {
        char sub[96];

        if (audio_state() == AUDIO_BUFFER) snprintf(sub, sizeof(sub), "缓冲中 · %s", s->artist);
        else if (g_app.paused) snprintf(sub, sizeof(sub), "已暂停 · %s", s->artist);
        else snprintf(sub, sizeof(sub), "%s", s->artist);
        gfx_text_ellipsis(FS_SM, 40, y + 19, 190, sub, C_PSUB);
    }

    {
        char t1[16], t2[16], line[48];

        fmt_time(t1, sizeof(t1), pos);
        fmt_time(t2, sizeof(t2), dur);
        snprintf(line, sizeof(line), "%s / %s", t1, t2);
        gfx_text_right(FS_SM, SCR_W - 14, y + 4, line, C_PSUB);

        if (audio_state() == AUDIO_BUFFER && audio_cache_pct() >= 0) {
            char b[32];

            snprintf(b, sizeof(b), "边下边播 %d%%", audio_cache_pct());
            gfx_text_right(FS_SM, SCR_W - 14, y + 19, b, C_ACC);
        } else if (audio_state() == AUDIO_ERROR) {
            gfx_text_right(FS_SM, SCR_W - 14, y + 19, "播放出错", C_WARN);
        } else if (g_app.now.id > 0 && cache_view_has(g_app.now.id)) {
            icon_download(SCR_W - 52, y + 24, 4, C_OK, 220);
            gfx_text_right(FS_SM, SCR_W - 56, y + 19, "已缓存", C_OK);
        } else {
            gfx_text_right(FS_SM, SCR_W - 14, y + 19, g_app.paused ? "○ 继续" : "△ 播放页",
                           C_PSUB);
        }
    }

    gfx_fill_rect(0, y + UI_PLAYER_H - 2, SCR_W, 2, gfx_rgb(0x3A, 0x3A, 0x3A));
    if (pct > 0) gfx_fill_rect(0, y + UI_PLAYER_H - 2, SCR_W * pct / 100, 2, C_HOT);
}

/* ------------------------------------------------------------------ */
/* 二次确认                                                            */
/* ------------------------------------------------------------------ */

static void confirm_draw(void)
{
    int w = 300, h = 96, x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
    const char *title = "确认？";
    const char *sub = "";

    switch (g_app.confirm_clear) {
    case 1: title = "清空全部缓存？"; sub = "会删掉所有已下载的歌和封面"; break;
    case 2: title = "清空下载队列？"; sub = "正在下载的任务也会停止"; break;
    case 3: title = "退出登录？"; sub = "会删除本地保存的 cookie"; break;
    case 4: title = "覆盖已导出的文件？"; sub = "同名文件会被替换"; break;
    case 5: title = "退出 MELODY？"; sub = ""; break;
    default: break;
    }

    gfx_fill_rect_a(0, 0, SCR_W, SCR_H, C_BLACK, 140);
    modal_fill_a(x + 2, y + 3, w, h, 10, C_BLACK, 60);
    modal_fill(x, y, w, h, 10, C_PANEL);
    modal_border(x, y, w, h, 10, C_LINE);
    gfx_text_center(FS_MD, x, w, y + 16, title, C_TXT);
    if (sub[0]) gfx_text_center(FS_SM, x, w, y + 42, sub, C_SUB);
    modal_fill(x + (w - 108) / 2, y + h - 28, 108, 20, 10, C_ACC);
    gfx_text_center(FS_SM, x + (w - 108) / 2, 108, y + h - 24, "○ 确定   × 取消", C_WHITE);
}

/* 启动时静默检查到新版本 → 模态弹窗：版本 + 更新日志 + 稍后/立即更新 */
static void upd_dialog_draw(void)
{
    int w = 400, h = 212, x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
    int st = api_update_state();
    const char *log = api_update_changelog();
    char line[128];
    int ly = y + 58;

    gfx_fill_rect_a(0, 0, SCR_W, SCR_H, C_BLACK, 150);
    modal_fill(x, y, w, h, 10, C_PANEL);
    modal_border(x, y, w, h, 10, C_LINE);

    snprintf(line, sizeof(line), st == 3 ? "发现新版本 v%s" : "新版本 v%s",
             api_update_version());
    gfx_text(FS_MD, x + 18, y + 12, line, C_TXT);
    snprintf(line, sizeof(line), "当前版本 v%s", APP_VER);
    gfx_text(FS_SM, x + 18, y + 34, line, C_MUTE);

    static char log_lines[192][112];
    int count=update_log_lines(log,log_lines,192);g_app.upd_scroll=update_log_clamp(g_app.upd_scroll,count);
    if(st==1)gfx_text(FS_SM,x+20,ly,"正在检查更新，请稍候…",C_SUB);
    else if(!count)gfx_text(FS_SM,x+20,ly,st==2?"当前已是最新版本":"暂无更新说明",C_MUTE);
    for(int i=0;i<UPDATE_LOG_VIEW_ROWS&&i+g_app.upd_scroll<count;++i)gfx_text(FS_SM,x+20,ly+i*16,log_lines[i+g_app.upd_scroll],C_SUB);
    if(count>UPDATE_LOG_VIEW_ROWS){int track=80,thumb=track*UPDATE_LOG_VIEW_ROWS/count;if(thumb<12)thumb=12;int ybar=ly+(track-thumb)*g_app.upd_scroll/(count-UPDATE_LOG_VIEW_ROWS);modal_fill(x+w-14,ly,3,track,1,C_PANEL2);modal_fill(x+w-14,ybar,3,thumb,1,C_ACC);}
    snprintf(line,sizeof(line),"说明 %d-%d / %d行  ↑↓滚动",count?g_app.upd_scroll+1:0,g_app.upd_scroll+5<count?g_app.upd_scroll+5:count,count);gfx_text(FS_SM,x+20,ly+84,line,C_MUTE);

    if (st == 4 || st==7) {
        int pct = api_update_progress();

        if(st==7)snprintf(line,sizeof(line),"已加入更新队列，等待当前请求结束");
        else snprintf(line, sizeof(line), "下载中 %d%%", pct);
        gfx_text(FS_SM, x + 20, y + h - 48, line, C_ACC);
        progress_bar(x + 20, y + h - 30, w - 40, 6, pct, C_ACC, C_PANEL2);
        gfx_text(FS_SM, x + 20, y + h - 18, "× 取消下载", C_MUTE);
    } else if (st == 5) {
        gfx_text(FS_SM, x + 20, y + h - 34, "下载完成，重启 MELODY 后生效", C_OK);
        modal_fill(x + w - 116, y + h - 40, 96, 26, 13, C_ACC);
        gfx_text_center(FS_SM, x + w - 116, 96, y + h - 34, "○ 好", C_WHITE);
    } else if (st == 6) {
        gfx_text_ellipsis(FS_SM, x + 20, y + h - 34, w - 140, api_update_message(), C_WARN);
        modal_fill(x + w - 116, y + h - 40, 96, 26, 13, C_ACC);
        gfx_text_center(FS_SM, x + w - 116, 96, y + h - 34, "○ 关闭", C_WHITE);
    } else if(st==3){
        int bw = 120;
        const char *a = "× 稍后";
        const char *b = "○ 立即更新";

        modal_fill(x + w - 2 * bw - 30, y + h - 40, bw, 26, 13,
                       g_app.upd_sel == 0 ? C_ACC : C_PANEL2);
        gfx_text_center(FS_SM, x + w - 2 * bw - 30, bw, y + h - 34, a,
                        g_app.upd_sel == 0 ? C_WHITE : C_SUB);
        modal_fill(x + w - bw - 16, y + h - 40, bw, 26, 13,
                       g_app.upd_sel == 1 ? C_ACC : C_PANEL2);
        gfx_text_center(FS_SM, x + w - bw - 16, bw, y + h - 34, b,
                        g_app.upd_sel == 1 ? C_WHITE : C_SUB);
    }else gfx_text(FS_SM,x+20,y+h-26,st==1?"× 返回（后台继续检查）":"○ / × 关闭",C_SUB);
}

/* ==== PART2 ==== */

/* ------------------------------------------------------------------ */
/* 初始化 / Toast                                                      */
/* ------------------------------------------------------------------ */

void ui_video_palette(UiVideoPalette*out){if(!out)return;out->panel=C_PANEL;out->accent=C_ACC;out->text=C_TXT;out->muted=C_MUTE;out->track=C_LINE;}

void ui_init(void)
{
    skin_runtime_init();
    theme_apply(g_app.theme);

    for (int i = 0; i < api_chart_count(); ++i) {
        Chart *c = (Chart *)api_chart(i);

        switch (c->icon) {
        case 0: c->c1 = gfx_rgb(0xFF, 0x7A, 0x4A); c->c2 = gfx_rgb(0xB3, 0x18, 0x1C); break;
        case 1: c->c1 = gfx_rgb(0x4A, 0x9E, 0xE0); c->c2 = gfx_rgb(0x1B, 0x3E, 0x7A); break;
        case 2: c->c1 = gfx_rgb(0xA6, 0x6B, 0xFF); c->c2 = gfx_rgb(0x4A, 0x1E, 0x8C); break;
        default: c->c1 = gfx_rgb(0x3E, 0xCF, 0x8E); c->c2 = gfx_rgb(0x18, 0x5A, 0x4E); break;
        }
    }
}

void ui_theme_changed(void)
{
    theme_apply(g_app.theme);
}

void ui_toast(const char *msg)
{
    snprintf(g_app.toast, sizeof(g_app.toast), "%s", msg);
    g_app.toast_t = 2.2f;
}

/* 选中项跟随滚动：把 sel 保持在可视区内 */
static void scroll_follow(float *scroll, int sel, int row_h, int view_h, float dt)
{
    int top = sel * row_h;
    int target = (int)*scroll;

    if (top < target + 2) target = top;
    if (top + row_h > target + view_h - 2) target = top + row_h - view_h + 2;
    if (target < 0) target = 0;

    *scroll = approach(*scroll, (float)target, 2200.0f, dt);
}

/* ------------------------------------------------------------------ */
/* 开机                                                                */
/* ------------------------------------------------------------------ */

static void screen_boot(float t)
{
    int cx = SCR_W / 2;
    int cy = 92;
    int a = (int)(255 * clamp01f(t / 0.5f));

    gfx_clear(C_PANEL);

    /* 红色圆盘 logo，外面两圈很淡的呼吸光环 */
    gfx_circle_a(cx, cy, 34 + (int)(3 * sinf(t * 2.2f)), C_ACC, 8);
    gfx_circle_a(cx, cy, 28, C_ACC, 12);
    gfx_circle(cx, cy, 24, BRAND_RED());
    icon_note(cx, cy + 1, 9, C_WHITE, 255);

    gfx_text_center(FS_LG, 0, SCR_W, 134, "网易云音乐", C_TXT);
    gfx_text_center(FS_SM, 0, SCR_W, 160, "MELODY PSP · 在线音乐", C_MUTE);

    {
        const char *msg = g_app.net_msg[0] ? g_app.net_msg : "正在连接网络";
        char buf[96];
        int dots = (int)(t * 2.5f) % 4;

        snprintf(buf, sizeof(buf), "%s", msg);
        for (int i = 0; i < dots && i < 3; ++i) {
            size_t n = strlen(buf);
            if (n < sizeof(buf) - 2) { buf[n] = '.'; buf[n + 1] = 0; }
        }
        gfx_text_center(FS_SM, 0, SCR_W, 196, buf, C_SUB);
    }

    {
        int w = 140;
        int phase = (int)(t * 90) % (w + 46);

        gfx_fill_round(cx - w / 2, 218, w, 3, 1, C_PANEL2);
        gfx_fill_round(cx - w / 2 + phase - 40, 218, 40, 3, 1, C_ACC);
    }

    gfx_text_center(FS_SM, 0, SCR_W, 246,
                    g_app.online ? "○ 网络设置 · SELECT 系统设置" : "○ 选择网络连接", C_MUTE);

    if (a < 255) gfx_fill_rect_a(0, 0, SCR_W, SCR_H, C_WHITE, 255 - a);
}

/* ------------------------------------------------------------------ */
/* 首页（发现）                                                        */
/* ------------------------------------------------------------------ */

#define HOME_ROW0_Y 34
#define HOME_H1_Y   98
#define HOME_CH_Y   122
#define HOME_CH_H   78
#define HOME_H2_Y   208
#define HOME_CV1_Y  232
#define HOME_CV2_Y  328
#define HOME_Q_Y    424
#define HOME_END    468

static const int home_row_y[5] = { HOME_ROW0_Y, HOME_CH_Y, HOME_CV1_Y, HOME_CV2_Y, HOME_Q_Y };
static const int home_row_h[5] = { 56, HOME_CH_H, 96, 96, 44 };

static void home_scroll_follow(float dt)
{
    int sel = g_app.home_sel;
    int row = 0;
    int top, bottom, target, view_top, view_bot;
    int maxscroll;

    for (int i = 0; i < 5; ++i) {
        int start = (i == 0) ? 0 : (i == 1 ? 1 : (i == 2 ? 4 : (i == 3 ? 9 : 14)));
        int len = (i == 0) ? 1 : (i == 1 ? 3 : (i == 2 || i == 3 ? 5 : 5));

        if (sel >= start && sel < start + len) { row = i; break; }
    }

    top = home_row_y[row];
    bottom = top + home_row_h[row];
    view_top = UI_CONTENT_TOP + 6;
    view_bot = content_bottom() - 6;

    target = (int)g_app.home_scroll;
    if (top - target < view_top) target = top - view_top;
    if (bottom - target > view_bot) target = bottom - view_bot;
    if (target < 0) target = 0;

    maxscroll = HOME_END - content_bottom();
    if (maxscroll < 0) maxscroll = 0;
    if (target > maxscroll) target = maxscroll;

    g_app.home_scroll = approach(g_app.home_scroll, (float)target, 1100.0f, dt);
}

static void home_hero(int y, int sel, float t)
{
    int x = 12, w = SCR_W - 24, h = 56;
    int pulse = (int)(20 + 12 * sinf(t * 2.0f));
    static const char *month[12] = { "1月", "2月", "3月", "4月", "5月", "6月",
                                     "7月", "8月", "9月", "10月", "11月", "12月" };
    static int day = 1, mon = 1;
    static float last_rtc;
    int day_i, mon_i;

    /* 日期一分钟查一次系统时钟就够了，不必每帧 syscall */
    if (t - last_rtc > 60.0f || last_rtc == 0.0f) {
        ScePspDateTime dt;

        last_rtc = t > 0 ? t : 0.01f;
        memset(&dt, 0, sizeof(dt));
        if (sceRtcGetCurrentClockLocalTime(&dt) >= 0) {
            day = dt.day;
            mon = clamp_i(dt.month, 1, 12);
        }
    }
    day_i = day;
    mon_i = mon;

    gfx_fill_round_grad(x, y, w, h, 8, C_ACC, gfx_lerp565(C_ACC, C_BLACK, 90));

    /* 左侧日期块 */
    gfx_fill_round_a(x, y, 56, h, 8, C_BLACK, 45);
    {
        char d[8];

        snprintf(d, sizeof(d), "%d", day_i);
        gfx_text_center(FS_XL, x, 56, y + 5, d, C_WHITE);
        gfx_text_center(FS_SM, x, 56, y + 39, month[mon_i - 1], gfx_rgb(255, 220, 220));
    }

    gfx_text(FS_MD, x + 68, y + 10, "每日推荐", C_WHITE);
    gfx_text(FS_SM, x + 68, y + 32, "根据你的口味生成 · 每天更新", gfx_rgb(255, 214, 214));

    /* 右侧播放圆钮 */
    gfx_circle_a(x + w - 30, y + h / 2, 17, C_WHITE, sel ? 52 : 28);
    gfx_circle_a(x + w - 30, y + h / 2, 15, C_WHITE, sel ? 30 : 16);
    icon_play(x + w - 28, y + h / 2, 6, C_WHITE, 255);

    if (sel) {
        gfx_round_border_a(x - 2, y - 2, w + 4, h + 4, 10, C_WHITE, 150 + pulse);
    }
}

static void chart_icon(int icon, int cx, int cy, unsigned short c)
{
    switch (icon) {
    case 0:
        gfx_fill_round(cx - 7, cy - 1, 3, 8, 1, c);
        gfx_fill_round(cx - 2, cy - 7, 3, 14, 1, c);
        gfx_fill_round(cx + 3, cy - 4, 3, 11, 1, c);
        break;
    case 1:
        gfx_tri(cx, cy - 8, cx - 7, cy, cx + 7, cy, c, 255);
        gfx_tri(cx, cy + 8, cx - 7, cy, cx + 7, cy, c, 255);
        break;
    case 2:
        gfx_tri(cx, cy - 9, cx - 8, cy + 1, cx + 8, cy + 1, c, 255);
        gfx_fill_round(cx - 3, cy + 1, 6, 8, 1, c);
        break;
    default:
        icon_search(cx, cy, 6, c, 255);
        break;
    }
}

static void home_chart(int i, int y, int sel, float t)
{
    const Chart *c = api_chart(i);
    int x = 12 + i * 156, w = 148, h = HOME_CH_H;
    char rank[8];
    (void)t;

    if (!c) return;

    gfx_fill_round_grad(x, y, w, h, 8, c->c1, c->c2);
    gfx_fill_round_a(x, y, w, h, 8, C_BLACK, sel ? 10 : 34);

    /* 背景大号 01/02/03 */
    snprintf(rank, sizeof(rank), "%02d", i + 1);
    gfx_fill_round_a(x + w - 50, y + 26, 44, 40, 4, C_BLACK, 55);
    gfx_text(FS_XL, x + w - 48, y + 28, rank, C_WHITE);

    gfx_circle_a(x + 30, y + 30, 18, C_WHITE, 22);
    chart_icon(c->icon, x + 30, y + 30, C_WHITE);

    gfx_text(FS_MD, x + 58, y + 16, c->name, C_WHITE);
    gfx_text(FS_SM, x + 58, y + 38, c->tag, gfx_rgb(255, 225, 225));
    gfx_text(FS_SM, x + 12, y + h - 18, "每天更新", gfx_rgb(255, 205, 205));

    if (sel) {
        gfx_round_border(x - 2, y - 2, w + 4, h + 4, 10, C_WHITE);
        gfx_fill_round_a(x - 2, y - 2, w + 4, h + 4, 10, C_BLACK, 16);
    }
}

static void home_cover(int idx, int x, int y, int sel)
{
    const CardList *cl = api_home_cards();
    const Card *k = (idx < cl->count) ? &cl->items[idx] : NULL;
    const unsigned short *px = NULL;
    char count[24] = "";

    if (k) {
        px = thumb_of(k->cover);
        fmt_count(count, sizeof(count), k->plays);
    }

    if (sel) gfx_round_border(x - 2, y - 2, UI_COVER_SZ + 4, UI_COVER_SZ + 4, 6, C_ACC);
    draw_cover(x, y, UI_COVER_SZ, 4, px, 112, 112);

    if (count[0]) {
        int cw = gfx_text_w(FS_SM, count);
        int tw = cw + 14;
        int tx = x + UI_COVER_SZ - tw - 4;

        gfx_fill_round_a(x + 4, y + UI_COVER_SZ - 18, UI_COVER_SZ - 8, 14, 3, C_BLACK, 115);
        icon_headset(x + UI_COVER_SZ - cw - 12, y + UI_COVER_SZ - 11, C_WHITE, 210);
        gfx_text(FS_SM, x + UI_COVER_SZ - cw - 5, y + UI_COVER_SZ - 17, count, C_WHITE);
        (void)tx;
    }

    if (k)
        gfx_text_ellipsis(FS_SM, x, y + UI_COVER_SZ + 4, UI_COVER_SZ, k->name,
                          sel ? C_ACC : C_TXT);
    else {
        gfx_fill_round(x + 8, y + UI_COVER_SZ + 8, 60, 6, 3, C_PANEL2);
    }
}

static void home_quick(int i, int y, int sel)
{
    static const char *labels[5] = { "歌单", "搜索", "下载", "缓存", "设置" };
    int x = 14 + i * 92, w = 84, h = 44;
    unsigned short ic = sel ? C_ACC : C_SUB;

    if (sel) {
        gfx_fill_round(x, y, w, h, 6, gfx_lerp565(C_PANEL2, C_ACC, 26));
        gfx_round_border(x, y, w, h, 6, C_ACC);
    } else {
        gfx_fill_round(x, y, w, h, 6, C_PANEL2);
        gfx_round_border_a(x, y, w, h, 6, C_LINE, 160);
    }

    switch (i) {
    case 0: icon_list(x + 18, y + h / 2, 6, ic, 230); break;
    case 1: icon_search(x + 17, y + h / 2, 6, ic, 230); break;
    case 2: icon_download(x + 18, y + h / 2, 5, ic, 230); break;
    case 3: icon_disk(x + 18, y + h / 2, 6, ic, 230); break;
    default: icon_gear(x + 18, y + h / 2, 6, ic, 230); break;
    }
    gfx_text(FS_SM, x + 34, y + h / 2 - 6, labels[i], sel ? C_ACC : C_TXT);
}

static void screen_home(float t, float dt)
{
    int n = api_chart_count();
    int top = UI_CONTENT_TOP;
    int scroll;
    const CardList *cl = api_home_cards();

    (void)n;
    panel_bg();
    nav_tabs();

    if (g_app.home_sel > HOME_ITEM_COUNT - 1) g_app.home_sel = 0;
    home_scroll_follow(dt);
    scroll = (int)g_app.home_scroll;

    gfx_set_clip(0, top, SCR_W, content_bottom());
    gfx_fill_rect(0, top, SCR_W, content_bottom() - top, C_PANEL);

    home_hero(HOME_ROW0_Y - scroll, g_app.home_sel == HOME_ITEM_HERO, t);

    section_header(12, HOME_H1_Y - scroll, SCR_W - 24, "排行榜", "每天更新");
    for (int i = 0; i < 3; ++i)
        home_chart(i, HOME_CH_Y - scroll, g_app.home_sel == HOME_ITEM_CHART + i, t);

    if (cl->count > 0) {
        char more[24];

        snprintf(more, sizeof(more), "%d 个", cl->count);
        section_header(12, HOME_H2_Y - scroll, SCR_W - 24, "推荐歌单", "更多");
        (void)more;
    } else {
        section_header(12, HOME_H2_Y - scroll, SCR_W - 24, "推荐歌单", "加载中");
    }

    for (int i = 0; i < 10; ++i) {
        int row = i / 5, col = i % 5;

        home_cover(i, 34 + col * 84, HOME_CV1_Y + row * 96 - scroll,
                   g_app.home_sel == HOME_ITEM_COVER + i);
    }

    for (int i = 0; i < 5; ++i)
        home_quick(i, HOME_Q_Y - scroll, g_app.home_sel == HOME_ITEM_QUICK + i);

    gfx_reset_clip();

    /* 右侧滚动条 */
    {
        int total = HOME_END, view = content_bottom() - top;

        scrollbar_draw(top, view, total, view, scroll);
    }

    api_request_thumbs();

    hint_begin();
    hint_item("○", "打开");
    hint_item("□", "最近");
    hint_item("△", "播放页");
    hint_item("L/R", "切换");
    hint_item("SELECT", "设置");
    hint_end();
}

/* ------------------------------------------------------------------ */
/* 列表（榜单 / 搜索 / 歌单 / 每日推荐 / 我喜欢的）                    */
/* ------------------------------------------------------------------ */

static void song_row(int x, int y, int w, int idx, const Song *s, int sel,
                     int playing, int cached, float t)
{
    if(g_app.download_select_mode){gfx_text(FS_SM,x+4,y+10,app_download_selected(s->id)?"[X]":"[ ]",C_ACC);x+=26;w-=26;}
    int idx_x = x + 26;
    int title_x = x + 34;
    int artist_x = x + 250;
    int badge_x = x + 374;
    int dur_x = x + w - 8;
    unsigned short title_col = C_TXT;
    /* No official URL (no copyright / region block): still playable through the
     * alternative sources, so it is dimmed, tagged, and not greyed out. */
    int alt_only = (s->playable == 2);
    int dim = alt_only;

    if (sel) row_focus(x, y, w, UI_ROW_H - 2);

    if (playing) {
        icon_bars(x + 16, y + UI_ROW_H / 2 - 1, 12, t, C_HOT);
    } else {
        char num[12];

        snprintf(num, sizeof(num), "%d", idx + 1);
        gfx_text_right(FS_SM, idx_x, y + 8, num, dim ? C_MUTE : C_SUB);
    }

    if (playing) title_col = C_HOT;
    else if (s->fee < 0) title_col = C_TXT;   /* 第三方源的行：正常颜色，不是灰的 */
    else if (alt_only) title_col = C_SUB;

    gfx_text_ellipsis(FS_MD, title_x, y + 6, 208, s->name, title_col);
    gfx_text_ellipsis(FS_SM, artist_x, y + 8, 118, s->artist, dim ? C_MUTE : C_SUB);

    {
        int bx = badge_x;

        if (s->hot) {
            tag_chip(bx, y + 7, "热门", C_HOT, 1);
            bx += gfx_text_w(FS_SM, "热门") + 12;
        } else if (s->fee < 0) {
            tag_chip(bx, y + 7, "酷我", C_ACC, 1);
            bx += gfx_text_w(FS_SM, "酷我") + 12;
        } else if (alt_only) {
            tag_chip(bx, y + 7, "三方", C_ACC, 1);
            bx += gfx_text_w(FS_SM, "三方") + 12;
        } else if (s->fee != 0) {
            tag_chip(bx, y + 7, "VIP", C_MUTE, 0);
            bx += gfx_text_w(FS_SM, "VIP") + 12;
        }
        if (s->mv > 0) tag_chip(bx, y + 7, "MV", C_ACC, 1);
    }

    if (cached) icon_download(x + w - 46, y + UI_ROW_H / 2 - 1, 4, C_OK, 220);

    {
        char dur[16];

        fmt_time(dur, sizeof(dur), s->dur_ms);
        gfx_text_right(FS_SM, dur_x, y + 8, dur, dim ? C_MUTE : C_MUTE);
    }
}

static void screen_list(float t, float dt)
{
    int kind = g_list.kind;
    int hero = (kind == LK_PLAYLIST || kind == LK_DAILY);
    int compact = hero && g_app.list_sel >= 6;
    int hero_h = compact ? 26 : 60;
    int top = UI_CONTENT_TOP + 4 + (hero ? hero_h + 2 : 14);
    int view_h = content_bottom() - top;
    char right[64];

    panel_bg();

    if (g_list.count == 0) snprintf(right, sizeof(right), "空列表");
    else if (g_app.list_total > g_list.count)
        snprintf(right, sizeof(right), "%d/%d 首", g_list.count, g_app.list_total);
    else snprintf(right, sizeof(right), "%d 首", g_list.count);
    nav_page(g_list.title[0] ? g_list.title : "列表", right, 1);

    api_request_thumbs();

    if (hero) {
        int y = UI_CONTENT_TOP + 4;

        if (kind == LK_DAILY) {
            gfx_fill_round_grad(12, y, SCR_W - 24, hero_h, 6, C_ACC,
                                gfx_lerp565(C_ACC, C_BLACK, 80));
            if (!compact) {
                gfx_text(FS_MD, 30, y + 12, "每日推荐", C_WHITE);
                gfx_text(FS_SM, 30, y + 36, "根据你的口味生成 · 每天 6:00 更新", C_WHITE);
                gfx_text_right(FS_SM, SCR_W - 30, y + 14, "今天", gfx_rgb(255, 220, 220));
            } else {
                gfx_text(FS_MD, 30, y + 5, "每日推荐", C_WHITE);
            }
            btn_red(SCR_W - 100, y + (hero_h - 20) / 2, 64, 20, "播放");
        } else {
            const Card *k = api_cards()->count > 0 ? &api_cards()->items[0] : NULL;
            const unsigned short *px = k ? thumb_of(k->cover) : NULL;
            int cs = compact ? 20 : 50;

            gfx_fill_round(12, y, SCR_W - 24, hero_h, 6, C_PANEL2);
            draw_cover(16, y + (hero_h - cs) / 2, cs, 3, px, 112, 112);

            gfx_text_ellipsis(FS_MD, 16 + cs + 8, y + (compact ? 5 : 8), SCR_W - 180,
                              g_list.title, C_TXT);
            if (!compact) {
                gfx_text_ellipsis(FS_SM, 16 + cs + 8, y + 30, 250,
                                  k && k->sub[0] ? k->sub : "网易云音乐", C_SUB);
                if (k && k->plays > 0) {
                    char b[24];

                    fmt_count(b, sizeof(b), k->plays);
                    gfx_text(FS_SM, 16 + cs + 8, y + 46, b, C_MUTE);
                    gfx_text(FS_SM, 16 + cs + 8 + gfx_text_w(FS_SM, b) + 4, y + 46,
                             "次播放", C_MUTE);
                }
            }
            btn_red(SCR_W - 104, y + (hero_h - 20) / 2, 76, 20, "播放全部");
        }
    } else {
        gfx_fill_rect(12, UI_CONTENT_TOP + 2, SCR_W - 24, 16, C_PANEL2);
        gfx_text(FS_SM, 34, UI_CONTENT_TOP + 4, "音乐标题", C_MUTE);
        gfx_text(FS_SM, 250, UI_CONTENT_TOP + 4, "歌手", C_MUTE);
        gfx_text_right(FS_SM, SCR_W - 16, UI_CONTENT_TOP + 4, "时长", C_MUTE);
    }

    scroll_follow(&g_app.list_scroll, g_app.list_sel, UI_ROW_H, view_h, dt);

    gfx_set_clip(0, top, SCR_W, content_bottom());

    for (int i = 0; i < g_list.count; ++i) {
        const Song *s = &g_list.songs[i];
        int y = top + i * UI_ROW_H - (int)g_app.list_scroll;
        int playing = (g_app.has_now && s->id == g_app.now.id);

        if (y < top - UI_ROW_H || y > content_bottom() + UI_ROW_H) continue;
        song_row(8, y, SCR_W - 16, i, s, i == g_app.list_sel, playing,
                 cache_view_has(s->id), t);
    }

    gfx_reset_clip();

    /* 列表尾部状态 */
    {
        int y = top + g_list.count * UI_ROW_H - (int)g_app.list_scroll;

        if (y < content_bottom() - 8) {
            if (g_app.list_more)
                gfx_text_center(FS_SM, 0, SCR_W, y + 8, "加载中...", C_MUTE);
            else if (g_list.count >= MAX_SONGS && g_app.list_total > g_list.count)
                gfx_text_center(FS_SM, 0, SCR_W, y + 8, "已到本机上限（600 首）", C_MUTE);
            else if (g_list.count > 0 && g_list.kind == LK_SEARCH)
                gfx_text_center(FS_SM, 0, SCR_W, y + 8, "— 已经到底了 —", C_MUTE);
        }
    }

    {
        int first = (int)g_app.list_scroll / UI_ROW_H;

        scrollbar_draw(top, view_h, g_list.count, view_h / UI_ROW_H + 1, first);
    }

    if(g_app.download_select_mode){char selected[64];snprintf(selected,sizeof(selected),"已选%d首  □选择  △后台下载",g_app.download_selected_count);gfx_text(FS_SM,20,content_bottom()-17,selected,C_ACC);}
    hint_begin();
    hint_item("L",g_app.download_select_mode?"退出多选":"多选下载");
    if(g_app.download_select_mode){hint_item("□","勾选");hint_item("△","下载选中");}
    else{hint_item("○","播放");hint_item("□","收藏");hint_item("△","下载");}
    if(g_list.kind==LK_PLAYLIST&&g_list.chart_id>0)hint_item("SELECT","收藏歌单");
    hint_item("×", "返回");
    hint_end();
}

/* ==== PART3 ==== */

/* ------------------------------------------------------------------ */
/* 播放页                                                              */
/* ------------------------------------------------------------------ */

static void marquee_draw(int x, int y, int w, const char *text, unsigned short color,
                         float phase)
{
    int tw = gfx_text_w(FS_LG, text);
    int old0, old1, old2, old3;

    if (tw <= w) {
        gfx_text(FS_LG, x, y, text, color);
        return;
    }

    gfx_get_clip(&old0, &old1, &old2, &old3);
    gfx_set_clip(x, y - 2, x + w, y + 24);

    {
        int span = tw + 36;
        int off = (int)(phase * 30.0f) % span;

        gfx_text(FS_LG, x - off, y, text, color);
        gfx_text(FS_LG, x - off + span, y, text, color);
    }

    gfx_set_clip(old0, old1, old2, old3);
}

static void player_backdrop(void)
{
    const unsigned short *bd = api_backdrop();

    if (bd) {
        gfx_blit565(0, 0, SCR_W, SCR_H, bd, SCR_W);
    } else {
        gfx_vgrad(0, 0, SCR_W, SCR_H, gfx_rgb(0x2A, 0x22, 0x22), gfx_rgb(0x0C, 0x0C, 0x0E));
    }
}

static void player_menu_draw(void)
{
    static const char *items[9] = { "看这首歌的评论", "播放模式", "音质",
                                    "显示歌词", "返回列表", "关闭菜单", "播放 MV", "下载 MV", "导出 MV" };
    int n = g_app.now.mv > 0 ? 9 : 6;
    int w=248,h=n*24+(is_metro_style()?42:30);
    int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;

    gfx_fill_rect_a(0, 0, SCR_W, SCR_H, C_BLACK, 130);
    modal_fill_a(x + 2, y + 3, w, h, 10, C_BLACK, 60);
    modal_fill(x, y, w, h, 10, C_PANEL);
    modal_border(x, y, w, h, 10, C_LINE);
    gfx_text(FS_SM, x + 14, y + 8, "功能", C_MUTE);

    for (int i = 0; i < n; ++i) {
        int iy = y + 24 + i * 24;
        int on = (i == g_app.menu_sel);

        if (on) {
            modal_fill(x + 6, iy - 2, w - 12, 22, 4, C_ROW);
            modal_fill(x + 8, iy + 3, 2, 12, 1, C_ACC);
        }
        gfx_text(FS_MD, x + 16, iy + 1, items[i], on ? C_ACC : C_TXT);

        if (i == 1) {
            static const char *m[3] = { "顺序", "随机", "单曲" };

            gfx_text_right(FS_SM, x + w - 16, iy + 4, m[g_app.play_mode % 3], C_MUTE);
        } else if (i == 2) {
            static const char *q[3] = { "128k", "192k", "320k" };

            gfx_text_right(FS_SM, x + w - 16, iy + 4, q[g_app.quality % 3], C_MUTE);
        }
    }
    if(is_metro_style())gfx_text(FS_SM,x+14,y+h-15,"↑↓选择  ○执行  ×关闭",C_MUTE);
}

/* 右上角时钟：HH:MM（本地时间） */
static int draw_clock(int x_right, int y, unsigned short color)
{
    ScePspDateTime dt;
    char buf[8];

    if (sceRtcGetCurrentClockLocalTime(&dt) < 0) return 0;
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)dt.hour, (int)dt.minute);
    gfx_text_right(FS_SM, x_right, y, buf, color);
    return 1;
}

static void screen_player(float t, float dt)
{
    const Song *s = &g_app.now;
    int dur = audio_dur_ms() > 0 ? audio_dur_ms() : s->dur_ms;
    int pos = audio_pos_ms();
    int bx = 176, bw = SCR_W - bx - 22;
    int liked = api_logged_in() ? g_app.is_liked : (g_app.has_now && store_is_fav(g_app.now.id));
    char t1[16], t2[16];

    panel_bg();
    player_backdrop();

    g_app.marquee += dt;
    if (gfx_text_w(FS_LG, s->name) <= bw) g_app.marquee = 0;

    /* 顶栏 */
    gfx_fill_rect_a(0, 0, SCR_W, UI_NAV_H, C_BLACK, 70);
    icon_chevron_left(12, 9, C_WHITE, 225);
    gfx_text(FS_SM, 26, 8, "正在播放", gfx_rgb(0xDD, 0xDD, 0xDD));
    {
        char right[32];

        snprintf(right, sizeof(right), "%dkbps", audio_kbps() > 0 ? audio_kbps() : 128);
        gfx_text_right(FS_SM, 268, 8, right, gfx_rgb(0xB0, 0xB0, 0xB0));
        /* The common status owner draws speed, clock and battery once. */
    }

    /* 封面 */
    {
        int size = 134;
        int aw = 0, ah = 0;
        const unsigned short *art = api_art(&aw, &ah);

        if (store_disc_player()) {
            static float disc_angle;
            /* UI frame time only: pause, buffering and stopped audio stay still. */
            if (g_app.has_now && !g_app.paused && audio_state() == AUDIO_PLAY &&
                dt > 0.0f && isfinite(dt)) {
                disc_angle += fminf(dt, 0.1f) * 18.0f;
                if (disc_angle >= 360.0f) disc_angle = fmodf(disc_angle, 360.0f);
            }
            skin_disc_draw(24, 54, size, disc_angle);
        } else {
            gfx_fill_round_a(22, 52, size + 4, size + 4, 10, C_BLACK, 50);
            draw_cover(24, 54, size, 8, art, aw, ah);
            if (art) gfx_round_border_a(24, 54, size, size, 8, C_WHITE, 26);
        }
    }

    /* 右侧信息 */
    marquee_draw(bx, 48, bw, s->name, C_WHITE, g_app.marquee);
    gfx_text_ellipsis(FS_SM, bx, 82, bw - 20, s->artist, gfx_rgb(0xCC, 0xCC, 0xCC));
    icon_heart(SCR_W - 30, 84, 7, liked ? C_HOT : gfx_rgb(0xCC, 0xCC, 0xCC), liked ? 255 : 170);

    gfx_text_ellipsis(FS_SM, bx, 100, bw, s->album[0] ? s->album : "网易云音乐",
                      gfx_rgb(0x96, 0x96, 0x96));

    /* 进度 */
    {
        int pct = dur > 0 ? (int)((long long)100 * pos / dur) : 0;
        int fillw;

        if (pct > 100) pct = 100;
        fillw = bw * pct / 100;

        gfx_fill_round(bx, 130, bw, 4, 2, gfx_rgb(0x55, 0x55, 0x55));
        if (fillw > 1) {
            gfx_fill_round(bx, 130, fillw, 4, 2, C_HOT);
            gfx_circle(bx + fillw, 132, 3, C_WHITE);
        }
        fmt_time(t1, sizeof(t1), pos);
        fmt_time(t2, sizeof(t2), dur);
        gfx_text(FS_SM, bx, 141, t1, gfx_rgb(0xB0, 0xB0, 0xB0));
        gfx_text_right(FS_SM, bx + bw, 141, t2, gfx_rgb(0x96, 0x96, 0x96));
    }

    /* 传输键 */
    {
        int cx = bx + bw / 2, cy = 184;

        icon_prev(bx + 52, cy, 8, gfx_rgb(0xDD, 0xDD, 0xDD), 230);
        icon_next(bx + bw - 52, cy, 8, gfx_rgb(0xDD, 0xDD, 0xDD), 230);

        gfx_circle_a(cx, cy, 24, C_HOT, 40);
        gfx_circle(cx, cy, 21, C_HOT);
        if (g_app.paused) icon_play(cx + 1, cy, 8, C_WHITE, 255);
        else icon_pause(cx, cy, 7, C_WHITE, 255);
    }

    /* 功能图标行 */
    {
        int iy = 216;
        int xs[4] = { bx + 24, bx + 88, bx + 152, bx + 216 };
        static const char *labels[4] = { "模式", "音质", "评论", "歌词" };
        static const int ic[4] = { 0, 0, 0, 0 };

        for (int i = 0; i < 4; ++i) {
            int cx = xs[i];

            (void)ic;
            switch (i) {
            case 0: icon_mode(cx, iy, g_app.play_mode, gfx_rgb(0xC8, 0xC8, 0xC8), 220); break;
            case 1: gfx_text_center(FS_SM, cx - 10, 20, iy - 6, "HQ",
                                    gfx_rgb(0xC8, 0xC8, 0xC8)); break;
            case 2: icon_comment(cx, iy, 6, gfx_rgb(0xC8, 0xC8, 0xC8), 220); break;
            default: gfx_text_center(FS_SM, cx - 10, 20, iy - 6, "词", gfx_rgb(0xC8, 0xC8, 0xC8)); break;
            }
            gfx_text_center(FS_SM, cx - 16, 32, iy + 9, labels[i], gfx_rgb(0x9A, 0x9A, 0x9A));
        }
    }

    /* 左下：音量 + 状态 */
    {
        int vx = 46, vw = 92;
        int vol = audio_volume();

        icon_volume(32, 208, gfx_rgb(0xC8, 0xC8, 0xC8), 220);
        gfx_fill_round(vx, 206, vw, 4, 2, gfx_rgb(0x55, 0x55, 0x55));
        if (vol > 0) gfx_fill_round(vx, 206, vw * vol / 100, 4, 2, gfx_rgb(0xDD, 0xDD, 0xDD));
        {
            char b[16];

            snprintf(b, sizeof(b), "%d%%", vol);
            gfx_text_right(FS_SM, 158, 202, b, gfx_rgb(0x9A, 0x9A, 0x9A));
        }
    }

    {
        int st = audio_state();
        int pct = audio_cache_pct();
        char msg[64];

        if (st == AUDIO_ERROR) {
            snprintf(msg, sizeof(msg), "%s", audio_error());
            gfx_text_ellipsis(FS_SM, 24, 228, 134, msg, C_WARN);
        } else if (st == AUDIO_END) {
            gfx_text(FS_SM, 24, 228, "播放结束", gfx_rgb(0x9A, 0x9A, 0x9A));
        } else if (g_app.paused) {
            gfx_text(FS_SM, 24, 228, "已暂停", gfx_rgb(0xD0, 0xD0, 0xD0));
        } else if (pct >= 0) {
            snprintf(msg, sizeof(msg), "边下边播 · %d%%", pct);
            gfx_text(FS_SM, 24, 228, msg, gfx_rgb(0xFF, 0xC0, 0xC0));
        } else {
            gfx_text(FS_SM, 24, 228, "已缓存 · 本地播放", C_OK);
        }
    }

    gfx_set_clip(0, 0, SCR_W, SCR_H);

    hint_begin();
    hint_item("△","歌词");
    hint_item("SELECT","菜单");
    hint_item("○",g_app.paused?"播放":"暂停");
    hint_item("L/R","切歌");
    hint_item("□","喜欢");
    hint_item("←→","进度");
    hint_end();
    (void)t;
}

/* ------------------------------------------------------------------ */
/* 歌词                                                                */
/* ------------------------------------------------------------------ */

static void screen_lyrics(float t, float dt)
{
    const Lyric *ly = api_lyric();
    const unsigned short *bd = api_backdrop_dim();
    int cur = -1;
    int pos = audio_pos_ms();
    float target = 0;
    int area_top = 48, area_bot = 226;

    (void)t;
    panel_bg();
    if (bd) gfx_blit565(0, 0, SCR_W, SCR_H, bd, SCR_W);
    else gfx_vgrad(0, 0, SCR_W, SCR_H, gfx_rgb(0x1A, 0x1A, 0x1E), gfx_rgb(0x08, 0x08, 0x0A));

    gfx_fill_rect_a(0, 0, SCR_W, 28, C_BLACK, 60);
    icon_chevron_left(12, 9, C_WHITE, 220);
    /* Reserve the same speed/clock/battery lanes as the player page. */
    gfx_text_ellipsis(FS_SM,26,8,236,g_app.now.name,gfx_rgb(0xE0,0xE0,0xE0));
    gfx_text_ellipsis(FS_SM,26,28,SCR_W-52,g_app.now.artist,gfx_rgb(0x9A,0x9A,0x9A));

    for (int i = 0; i < ly->count; ++i)
        if (pos >= ly->ms[i]) cur = i;

    int line_h=g_app.lyric_translation?48:30;
    if (cur >= 0) target = (float)cur * line_h;
    int gap_ms = cur >= 0 && cur + 1 < ly->count ? ly->ms[cur + 1] - ly->ms[cur] : 3000;
    g_app.lyric_scroll = lyric_scroll_position(g_app.lyric_scroll / line_h,
                                               target / line_h, dt, gap_ms) * line_h;
    if (cur < 0) g_app.lyric_scroll = 0;

    gfx_set_clip(0, area_top, SCR_W, area_bot);

    if (ly->count == 0) {
        gfx_text_center(FS_MD, 0, SCR_W, 118, "暂无歌词", gfx_rgb(0x90, 0x90, 0x90));
        gfx_text_center(FS_SM, 0, SCR_W, 142, "这首歌没有歌词，或者还没加载完",
                        gfx_rgb(0x70, 0x70, 0x74));
    } else {
        for (int i = 0; i < ly->count; ++i) {
            int y = 120 + (int)((float)i * line_h - g_app.lyric_scroll);
            int dist = i - cur;
            int alpha;
            unsigned short col;

            if (y < area_top - 30 || y > area_bot + 10) continue;
            if (i == cur) {
                col = C_WHITE;
                alpha = 255;
            } else if (dist < 0) {
                col = gfx_rgb(0x78, 0x78, 0x7C);
                alpha = 140;
            } else {
                col = gfx_rgb(0xA8, 0xA8, 0xAC);
                alpha = 235 - (dist > 5 ? 110 : dist * 14);
            }

            {
                int tw = gfx_text_w(FS_MD, ly->text[i]);
                int tx = SCR_W / 2 - tw / 2;

                if (i == cur) {
                    /* Real YRC spans reveal glyph pixels; ordinary LRC stays white. */
                    if (lyric_highlight_width(FS_MD, ly->text[i], i, pos) < 0)
                        gfx_text_a(FS_MD, tx, y, ly->text[i], C_WHITE, 255);
                    else
                        lyric_draw_text(tx, y, tw, ly->text[i], FS_MD, C_MUTE, C_HOT, i, pos, 0);
                } else {
                    gfx_text_a(FS_MD, tx, y, ly->text[i], col, alpha);
                }
                if(g_app.lyric_translation&&ly->translation[i][0])gfx_text_center(FS_SM,20,SCR_W-40,y+23,ly->translation[i],i==cur?C_WHITE:C_MUTE);
            }
        }
    }

    gfx_reset_clip();

    {
        int dur = audio_dur_ms();
        int w = SCR_W - 160;
        int filled = dur > 0 ? w * pos / dur : 0;
        char b1[16], b2[16];

        if (filled > w) filled = w;
        gfx_fill_round(80, 236, w, 3, 1, gfx_rgb(0x55, 0x55, 0x55));
        if (filled > 1) gfx_fill_round(80, 236, filled, 3, 1, C_HOT);
        fmt_time(b1, sizeof(b1), pos);
        fmt_time(b2, sizeof(b2), dur);
        gfx_text(FS_SM, 80, 242, b1, gfx_rgb(0x90, 0x90, 0x90));
        gfx_text_right(FS_SM, 80 + w, 242, b2, gfx_rgb(0x90, 0x90, 0x90));
    }

    hint_begin();
    hint_item("△", "返回");
    hint_item("○", g_app.paused ? "播放" : "暂停");
    hint_item("□",g_app.lyric_translation?"原文":"翻译");
    hint_item("L/R", "切歌");
    hint_item("←→", "进度");
    hint_item("↑↓", "音量");
    hint_item("×", "列表");
    hint_end();
}

/* ------------------------------------------------------------------ */
/* 搜索                                                                */
/* ------------------------------------------------------------------ */

static const char *g_kb0 = "qwertyuiop";
static const char *g_kb1 = "asdfghjkl";
static const char *g_kb2 = "zxcvbnm";
static const char *g_chips[5] = { "周杰伦", "林俊杰", "邓紫棋", "陈奕迅", "薛之谦" };

#define KB_X0   16
#define KB_KW   42
#define KB_KGAP 3
#define KB_KH   25
#define KB_Y0   120

static void kb_key_rect(int row, int col, int *x, int *y, int *w)
{
    *y = KB_Y0 + row * (KB_KH + 3);
    if (row < 2) {
        *x = KB_X0 + col * (KB_KW + KB_KGAP);
        *w = KB_KW;
    } else {
        if (col < 7) {
            *x = KB_X0 + col * (KB_KW + KB_KGAP);
            *w = KB_KW;
        } else if (col == 7) {
            *x = KB_X0 + 7 * (KB_KW + KB_KGAP);
            *w = 76;
        } else {
            *x = KB_X0 + 7 * (KB_KW + KB_KGAP) + 76 + 4;
            *w = 53;
        }
    }
}

static const char *kb_col_label(int row, int col)
{
    static char one[2];

    one[1] = 0;
    if (row == 0) {
        if (col >= 10) return NULL;
        one[0] = g_kb0[col];
        return one;
    }
    if (row == 1) {
        if (col < 9) { one[0] = g_kb1[col]; return one; }
        return "←";
    }
    if (col < 7) { one[0] = g_kb2[col]; return one; }
    if (col == 7) return "空格";
    return "搜索";
}

static void search_chip_row(int y, const char *label, int row, int count,
                           const char *const *items, int selected, int ime_mode)
{
    int x = 15;

    (void)ime_mode;
    if (label) {
        gfx_text(FS_SM, 15, y + 5, label, C_MUTE);
        x = 15 + gfx_text_w(FS_SM, label) + 8;
    }
    for (int i = 0; i < count; ++i) {
        int w = 88, kx = x + i * (w + 4);
        int sel = (g_app.kb_row == row && selected == i);

        if (kx + w > SCR_W - 10) break;
        gfx_fill_round(kx, y, w, 20, 10, sel ? C_ACC : C_PANEL2);
        if (!sel) gfx_round_border_a(kx, y, w, 20, 10, C_LINE, 160);
        gfx_text_ellipsis(FS_SM, kx + 7, y + 4, w - 12, items[i], sel ? C_WHITE : C_SUB);
    }
}

static void screen_search(float t, float dt)
{
    char right[40];

    panel_bg();

    if(g_app.search_state==2)snprintf(right,sizeof(right),g_app.search_kind?"%d 项结果":"%d 首结果",g_app.search_kind?api_search_entities()->count:g_list.count);
    else if (g_app.search_state == 1) snprintf(right, sizeof(right), "搜索中");
    else right[0] = 0;

    nav_page("搜索", right, 1);
    if(!g_app.kb_edit)for(int kind=0;kind<5;++kind){int x=18+kind*92;gfx_text(FS_SM,x,63,api_search_category_name(kind),kind==g_app.search_kind?C_ACC:C_MUTE);if(kind==g_app.search_kind)gfx_fill_rect(x,78,38,2,C_ACC);}

    /* 搜索框：已上屏文本 + 输入法 preedit（强调色）+ 模式角标 */
    {
        int x = 12, y = 34, w = SCR_W - 24, h = 30;
        int focused = g_app.kb_edit;
        int ime = focused && ime_active();
        const char *pre = ime ? ime_preedit() : "";

        gfx_fill_round(x, y, w, h, 15, C_PANEL2);
        gfx_round_border(x, y, w, h, 15, focused ? C_ACC : C_LINE);
        icon_search(x + 18, y + 15, 6, focused ? C_ACC : C_MUTE, 230);

        if (g_app.query_len > 0 || pre[0]) {
            int tx = x + 34;
            int qw = gfx_text_w(FS_MD, g_app.query);

            if (g_app.query_len > 0) gfx_text(FS_MD, tx, y + 7, g_app.query, C_TXT);
            if (pre[0]) {
                gfx_text(FS_MD, tx + qw, y + 7, pre, C_ACC);
                if (focused && ((int)(t * 2.0f) & 1))
                    gfx_fill_rect(tx + qw + gfx_text_w(FS_MD, pre) + 1, y + 7, 2, 16, C_ACC);
            } else if (focused && ((int)(t * 2.0f) & 1)) {
                gfx_fill_rect(tx + qw + 1, y + 7, 2, 16, C_ACC);
            }
        } else {
            gfx_text(FS_SM, x + 34, y + 9, "搜歌曲/歌手/歌单（拼音或假名）", C_MUTE);
        }
        if (focused) {
            const char *mode = ime_mode() == IME_KANA ? "假名" : "拼音";
            int mw = gfx_text_w(FS_SM, mode);

            gfx_fill_round(x + w - mw - 22, y + 8, mw + 14, 15, 7, C_ACC);
            gfx_text(FS_SM, x + w - mw - 15, y + 9, mode, C_WHITE);
        } else if (g_app.query_len > 0) {
            gfx_circle_a(x + w - 16, y + h / 2, 8, C_MUTE, 200);
            gfx_text_center(FS_SM, x + w - 24, 16, y + h / 2 - 6, "×", C_WHITE);
        }
    }

    if(!g_app.kb_edit&&g_app.search_kind){
        const CardList*results=api_search_entities();int top=86,row=42,view=content_bottom()-top;
        scroll_follow(&g_app.list_scroll,g_app.list_sel,row,view,dt);gfx_set_clip(0,top,SCR_W,content_bottom());
        for(int i=0;i<results->count;++i){const Card*c=&results->items[i];int y=top+i*row-(int)g_app.list_scroll;if(y<top-row||y>content_bottom())continue;if(i==g_app.list_sel)row_focus(8,y,SCR_W-16,row-2);gfx_text_ellipsis(FS_MD,20,y+3,425,c->name,i==g_app.list_sel?C_ACC:C_TXT);char detail[96];snprintf(detail,sizeof(detail),"%s  %d首",c->sub,c->tracks);gfx_text_ellipsis(FS_SM,20,y+24,425,detail,C_MUTE);}
        gfx_reset_clip();if(!results->count)gfx_text_center(FS_SM,0,SCR_W,130,g_app.search_state==1?"正在搜索分类…":"此分类暂无结果，或服务端限制访问",C_MUTE);
        scrollbar_draw(top,view,results->count,view/row,(int)g_app.list_scroll/row);hint_begin();hint_item("L/R","分类");hint_item("○","打开");hint_item("△","改搜索");hint_item("×","返回");hint_end();return;
    }

    if (g_app.kb_edit) {
        int ime = ime_active();
        int ime_n = ime ? ime_candidate_count() : 0;
        int sug_n = api_suggest_count();
        int top_n = g_app.assoc_n > 0 ? g_app.assoc_n
                  : (sug_n > 0 ? sug_n : (ime ? 0 : g_app.chips_n));
        int left_x = 15, right_x = 352, right_w = 116;

        /* 上排：联想（上屏后 / 在线联想）或推荐；下排：输入法候选（可左右滚动） */
        if (top_n > 0) {
            const char *label = g_app.assoc_n > 0 ? "联想" : (sug_n > 0 ? "联想" : "推荐");
            int win = 3;

            if (g_app.chip_off > top_n - 1) g_app.chip_off = top_n > win ? top_n - win : 0;
            gfx_text(FS_SM, left_x, 73, label, C_MUTE);
            if (g_app.chip_off > 0) gfx_text(FS_SM, left_x + 30, 73, "<", C_ACC);
            for (int i = 0; i < win && g_app.chip_off + i < top_n; ++i) {
                int idx = g_app.chip_off + i;
                const char *txt = g_app.assoc_n > 0 ? g_app.assoc[idx]
                                : (sug_n > 0 ? api_suggest(idx) : g_app.chips[idx]);
                int x = left_x + 44 + i * 96, y = 68, w = 92;
                int sel = (g_app.kb_row == -1 && g_app.chip_sel == idx);

                if (!txt || !txt[0]) continue;
                gfx_fill_round(x, y, w, 20, 10, sel ? C_ACC : C_PANEL2);
                if (!sel) gfx_round_border_a(x, y, w, 20, 10, C_LINE, 160);
                gfx_text_ellipsis(FS_SM, x + 6, y + 4, w - 10, txt, sel ? C_WHITE : C_TXT);
            }
            if (g_app.chip_off + win < top_n) gfx_text(FS_SM, left_x + 44 + win * 96, 73, ">", C_ACC);
        }

        if (ime_n > 0) {
            int win = 4;                 /* 一屏 4 个，左右滚动 */
            int off = g_app.ime_off;

            gfx_text(FS_SM, left_x, 99, "候选", C_MUTE);
            if (off > 0) gfx_text(FS_SM, left_x + 30, 99, "<", C_ACC);
            for (int i = 0; i < win && off + i < ime_n; ++i) {
                char tag[40];
                int idx = off + i;
                int x = left_x + 46 + i * 68, y = 94, w = 64;
                int sel = (g_app.kb_row == -2 && g_app.ime_sel == idx);

                snprintf(tag, sizeof(tag), "%s", ime_candidate(idx));
                if (!tag[0]) continue;
                gfx_fill_round(x, y, w, 20, 10, sel ? C_ACC : C_PANEL2);
                if (!sel) gfx_round_border_a(x, y, w, 20, 10, C_LINE, 160);
                gfx_text_ellipsis(FS_SM, x + 5, y + 4, w - 8, tag, sel ? C_WHITE : C_TXT);
            }
            if (off + win < ime_n) gfx_text(FS_SM, left_x + 46 + win * 68, 99, ">", C_ACC);
        }

        /* 右列：搜索历史（竖向，可选中搜索） */
        {
            int hn = search_hist_count();
            int shown = hn > 4 ? 4 : hn;

            gfx_text(FS_SM, right_x, 66, "历史", C_MUTE);
            for (int i = 0; i < shown; ++i) {
                int y = 80 + i * 16;
                int sel = (g_app.kb_row == -3 && g_app.hist_sel == i);

                if (sel) gfx_fill_round(right_x - 3, y, right_w, 15, 5, C_ACC);
                else gfx_round_border_a(right_x - 3, y, right_w, 15, 5, C_LINE, 130);
                gfx_text_ellipsis(FS_SM, right_x + 3, y + 2, right_w - 8,
                                  search_hist_at(i), sel ? C_WHITE : C_SUB);
            }
            if (hn == 0) gfx_text(FS_SM, right_x, 84, "暂无记录", C_MUTE);
        }

        /* 键盘 */
        for (int row = 0; row < 3; ++row) {
            int cols = (row == 2) ? 9 : 10;

            for (int col = 0; col < cols; ++col) {
                int x, y, w;
                const char *label;
                int sel;

                kb_key_rect(row, col, &x, &y, &w);
                label = kb_col_label(row, col);
                if (!label) continue;
                sel = (g_app.kb_row == row && g_app.kb_col == col);

                if (row == 2 && col == 8) {
                    gfx_fill_round(x, y, w, KB_KH, 5, sel ? C_HOT : C_ACC);
                    gfx_text_center(FS_SM, x, w, y + 5, label, C_WHITE);
                } else if (row == 2 && col == 7) {
                    gfx_fill_round(x, y, w, KB_KH, 5, sel ? C_ACC : C_PANEL2);
                    if (!sel) gfx_round_border_a(x, y, w, KB_KH, 5, C_LINE, 160);
                    gfx_text_center(FS_SM, x, w, y + 5, label, sel ? C_WHITE : C_SUB);
                } else {
                    gfx_fill_round(x, y, w, KB_KH, 5, sel ? C_ACC : C_PANEL2);
                    if (!sel) gfx_round_border_a(x, y, w, KB_KH, 5, C_LINE, 170);
                    gfx_text_center(FS_MD, x, w, y + 4, label, sel ? C_WHITE : C_TXT);
                }
            }
        }

        {
            const char *tip = ime_n > 0 ? "○ 上屏 · △ 联想 · □ 中/日/英 · ← 删除"
                                        : "↑↓←→ 选键 · ○ 确认 · △ 联想 · □ 中/日";

            if (g_app.search_state == 1) tip = "正在搜索 ...";
            else if (g_app.search_state == 3) tip = "没有结果，换个拼音/假名试试";

            gfx_text_center(FS_SM, 0, SCR_W, 205, tip,
                            g_app.search_state == 3 ? C_WARN : C_MUTE);
        }
    } else {
        int top = 86, row_h = UI_ROW_H, view_h = content_bottom() - top - 2;

        scroll_follow(&g_app.list_scroll, g_app.list_sel, row_h, view_h, dt);

        if (g_app.search_state == 1) {
            gfx_text_center(FS_SM, 0, SCR_W, 120, "正在搜索 ...", C_SUB);
            spinner_draw(SCR_W / 2, 146, g_app.spinner);
        } else if (g_list.count == 0) {
            gfx_text_center(FS_MD, 0, SCR_W, 112, "没有找到歌曲", C_SUB);
            gfx_text_center(FS_SM, 0, SCR_W, 136, "试试输入全拼（如 zhoujielun）或假名", C_MUTE);
        }

        gfx_set_clip(0, top, SCR_W, content_bottom());
        for (int i = 0; i < g_list.count; ++i) {
            const Song *s = &g_list.songs[i];
            int y = top + i * row_h - (int)g_app.list_scroll;
            int playing = (g_app.has_now && s->id == g_app.now.id);

            if (y < top - row_h || y > content_bottom() + row_h) continue;
            song_row(8, y, SCR_W - 16, i, s, i == g_app.list_sel, playing,
                     cache_view_has(s->id), t);
        }
        gfx_reset_clip();

        if (g_app.list_more)
            gfx_text_center(FS_SM, 0, SCR_W, content_bottom() - 16, "加载中 ...", C_MUTE);

        scrollbar_draw(top, view_h, g_list.count, view_h / row_h + 1,
                       (int)g_app.list_scroll / row_h);

        hint_begin();
        hint_item("L/R", "分类");
        hint_item("○", "播放");
        hint_item("△", "改搜索");
        hint_item("□", "下载");
        hint_item("SELECT", "输入法");
        hint_end();
    }
}

/* ==== PART4 ==== */

/* ------------------------------------------------------------------ */
/* 下载管理                                                            */
/* ------------------------------------------------------------------ */

static const char *dl_state_text(const DlItem *it, char *buf, int len)
{
    switch (it->state) {
    case DL_ACTIVE:
        snprintf(buf, (size_t)len, "下载中 %d%%", it->pct < 0 ? 0 : (it->pct > 99 ? 99 : it->pct));
        return buf;
    case DL_PAUSED:
        snprintf(buf,(size_t)len,"已暂停 %d%%",it->pct < 0 ? 0 : (it->pct > 99 ? 99 : it->pct));return buf;
    case DL_DONE:
        return "已完成";
    case DL_FAILED:
        return "失败";
    default:
        return "等待中";
    }
}

static void screen_download_mv(float t, float dt);

static void dl_tabs(int y)
{
    static const char *names[2] = { "歌曲", "MV" };
    int x = 14;

    for (int i = 0; i < 2; ++i) {
        int w = gfx_text_w(FS_MD, names[i]) + 20;
        int on = (i == g_app.dl_tab);

        if (on) {
            gfx_fill_round(x, y, w, 20, 10, C_ACC);
            gfx_text_center(FS_MD, x, w, y + 2, names[i], C_WHITE);
        } else {
            gfx_fill_round(x, y, w, 20, 10, C_PANEL2);
            gfx_text_center(FS_MD, x, w, y + 2, names[i], C_SUB);
        }
        x += w + 6;
    }
}

static void download_summary(int song_count,int mv_count){
    char speed[24],detail[72];
    gfx_fill_round(12,34,SCR_W-24,50,8,C_PANEL2);
    gfx_text(FS_SM,24,40,"当前总网速",C_MUTE);
    snprintf(speed,sizeof(speed),"%d",net_speed_kbps());gfx_text(FS_XL,24,50,speed,C_ACC);
    gfx_text(FS_SM,24+gfx_text_w(FS_XL,speed)+4,66,"KB/s",C_SUB);
    snprintf(detail,sizeof(detail),"歌曲任务 %d · MV %d",song_count,mv_count);gfx_text_right(FS_SM,SCR_W-24,42,detail,C_SUB);
    if(mvx_active())snprintf(detail,sizeof(detail),"MV导出 %d%%",mvx_pct());
    else if(api_mv_download_state()==1)snprintf(detail,sizeof(detail),"MV下载 %d%%",api_mv_download_pct());
    else snprintf(detail,sizeof(detail),"两页共用统计 · 含播放与图片流量");
    gfx_text_right(FS_SM,SCR_W-24,62,detail,C_MUTE);
}
static void screen_net_speed(void){
    if(store_show_speed()){char speed[24];snprintf(speed,sizeof(speed),"%d KB/s",net_speed_kbps());gfx_text_right(FS_SM,350,8,speed,C_NAV_DIM);}
    nav_status_draw();
    if(is_metro_style()&&g_app.show_mem){char memory[24];snprintf(memory,sizeof(memory),"%.1f/%.0fM",sysi_used_kb()/1024.f,(sysi_used_kb()+sysi_free_kb())/1024.f);gfx_text_right(FS_SM,store_show_speed()?286:354,8,memory,C_NAV_DIM);}
}
static void export_progress_draw(void){
    if(!mvx_active())return;
    int pct=mvx_pct();if(pct<0)pct=0;if(pct>100)pct=100;
    char label[64];snprintf(label,sizeof(label),"正在导出MV %d%% · 原缓存保留",pct);
    modal_fill(24,188,SCR_W-48,48,7,C_PANEL);
    gfx_text(FS_SM,36,196,label,C_TXT);gfx_fill_rect(36,218,SCR_W-72,6,C_LINE);
    gfx_fill_rect(36,218,(SCR_W-72)*pct/100,6,C_ACC);
}

static void screen_download(float t, float dt)
{
    int n = g_dl_view_n;
    int top = 112;
    int view_h = content_bottom() - top;
    int active = 0, done = 0;
    char right[64];

    if (g_app.dl_tab) {
        screen_download_mv(t, dt);
        return;
    }

    panel_bg();

    for (int i = 0; i < n; ++i) {
        if (g_dl_view[i].state == DL_ACTIVE) active++;
        else if (g_dl_view[i].state == DL_DONE) done++;
    }

    snprintf(right, sizeof(right), "%d 个任务", n);
    nav_page("下载管理", right, 1);
    dl_tabs(86);

    MvDlItem mv_items[MV_DL_MAX];
    download_summary(n,mv_dl_snapshot(mv_items,MV_DL_MAX));

    if (n == 0) {
        gfx_circle_a(SCR_W / 2, 126, 22, C_PANEL2, 255);
        icon_download(SCR_W / 2, 125, 9, C_MUTE, 220);
        gfx_text_center(FS_MD, 0, SCR_W, 156, "下载队列是空的", C_SUB);
        gfx_text_center(FS_SM, 0, SCR_W, 180, "在列表里按 △ 把歌存到记忆棒", C_MUTE);
        gfx_text_center(FS_SM, 0, SCR_W, 196, "缓存里的歌不联网也能听", C_MUTE);
        gfx_text_center(FS_SM, 0, SCR_W, 216, "按 L/R 查看 MV 下载", C_ACC);
    }

    scroll_follow(&g_app.list_scroll, g_app.dl_sel, 46, view_h, dt);

    gfx_set_clip(0, top, SCR_W, content_bottom());
    for (int i = 0; i < n; ++i) {
        const DlItem *it = &g_dl_view[i];
        int y = top + i * 46 - (int)g_app.list_scroll;
        int sel = (i == g_app.dl_sel);
        char state[32];
        char sz[24];
        int pct = 0;
        unsigned short col = C_SUB;

        if (y < top - 46 || y > content_bottom() + 46) continue;

        if (sel) row_focus(8, y + 2, SCR_W - 16, 42);

        gfx_text_ellipsis(FS_MD, 20, y + 6, SCR_W - 190, it->song.name, C_TXT);
        gfx_text_ellipsis(FS_SM, 20, y + 26, SCR_W - 230, it->song.artist, C_MUTE);
        if (it->song.mv > 0) tag_chip(SCR_W - 150, y + 7, "MV", C_ACC, 1);

        if (it->state == DL_ACTIVE) { pct = it->pct; col = C_ACC; }
        else if (it->state == DL_DONE) { pct = 100; col = C_OK; }
        else if (it->state == DL_FAILED) { col = C_WARN; }else if(it->state==DL_PAUSED){pct=it->pct;col=C_MUTE;}
        else { col = C_MUTE; }
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;

        {
            const char *st = dl_state_text(it, state, sizeof(state));
            int tw = gfx_text_w(FS_SM, st);

            if (it->state == DL_DONE)
                icon_check(SCR_W - 20 - tw - 12, y + 13, C_OK, 255);
            gfx_text_right(FS_SM, SCR_W - 20, y + 6, st, col);
        }

        /* 进行中的行显示「已下载/总数」；这个数字由同一个百分比反算，不会再出现
         * 进度条 100% 而文字 1.8/4.0MB 这种自相矛盾。 */
        if (it->state == DL_ACTIVE || it->state == DL_PAUSED)
            fmt_done_total(sz, sizeof(sz), dl_bytes_from_pct(pct, it->total), it->total);
        else
            fmt_size(sz, sizeof(sz), it->total > 0 ? it->total : 0);
        gfx_text_right(FS_SM, SCR_W - 20, y + 26, sz, C_MUTE);

        progress_bar(20, y + 40, SCR_W - 40, 3, pct, col, C_PANEL2);
    }
    gfx_reset_clip();

    {
        int first = (int)g_app.list_scroll / 46;

        scrollbar_draw(top, view_h, n, view_h / 46 + 1, first);
    }

    hint_begin();
    hint_item("○","暂停/继续");
    hint_item("△","移除任务");
    hint_item("□","清空");
    hint_item("L/R", "MV");
    hint_item("SELECT", "播放");
    hint_item("×", "返回");
    hint_end();
    (void)t;
}

static void screen_download_mv(float t, float dt)
{
    MvDlItem items[MV_DL_MAX];
    int n = mv_dl_snapshot(items, MV_DL_MAX);   /* 读 cache/mv_index.txt，不扫目录 */
    int top = 112;
    int view_h = content_bottom() - top;
    int active = api_mv_download_state();
    char right[64], b[64];

    panel_bg();
    snprintf(right, sizeof(right), "%d 个 MV", n);
    nav_page("下载管理", right, 1);
    dl_tabs(86);

    download_summary(g_dl_view_n,n);

    if (n == 0) {
        gfx_circle_a(SCR_W / 2, 140, 22, C_PANEL2, 255);
        icon_download(SCR_W / 2, 139, 9, C_MUTE, 220);
        gfx_text_center(FS_MD, 0, SCR_W, 170, "还没有 MV 或本地视频", C_SUB);
        gfx_text_center(FS_SM, 0, SCR_W, 194, "本地视频默认不列出；可在设置→本地 MV 搜索开启", C_MUTE);
        gfx_text_center(FS_SM, 0, SCR_W, 210, "下载在后台继续，完成后在这里播放", C_MUTE);
    }

    scroll_follow(&g_app.list_scroll, g_app.dl_sel, 46, view_h, dt);

    gfx_set_clip(0, top, SCR_W, content_bottom());
    for (int i = 0; i < n; ++i) {
        const MvDlItem *it = &items[i];
        int y = top + i * 46 - (int)g_app.list_scroll;
        int sel = (i == g_app.dl_sel);
        /* Only the MV that is actually being fetched may report live progress.
         * Bytes come from the download source, never from percent*total — that
         * combination could print "11.5/11.5MB" while the bar showed 33%. */
        int live = it->state == 1 && it->mvid > 0 && it->mvid == api_mv_info()->id;
        long long done_bytes = live ? api_mv_download_bytes() : 0;
        long long total_bytes = live ? api_mv_download_total() : 0;
        int pct = live ? (total_bytes > 0 ? (int)(done_bytes * 100 / total_bytes) : 0) : it->pct;
        unsigned short col = live ? C_ACC : it->state==3?C_WARN:C_OK;
        char sz[24];

        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (y < top - 46 || y > content_bottom() + 46) continue;
        if (sel) row_focus(8, y + 2, SCR_W - 16, 42);

        gfx_text_ellipsis(FS_MD, 20, y + 6, SCR_W - 190, it->name, C_TXT);
        gfx_text_ellipsis(FS_SM, 20, y + 26, SCR_W - 230, it->artist, C_MUTE);
        gfx_text_right(FS_SM, SCR_W - 20, y + 6, live ? "下载中" : it->state==3?"未完成 · ○继续":"已完成", col);
        if (live && total_bytes > 0) fmt_done_total(sz, sizeof(sz), done_bytes, total_bytes);
        else if (live) fmt_size(sz, sizeof(sz), done_bytes);
        else fmt_size(sz, sizeof(sz), it->size > 0 ? it->size : 0);
        gfx_text_right(FS_SM, SCR_W - 20, y + 26, sz, C_MUTE);
        progress_bar(20, y + 40, SCR_W - 40, 3, pct, col, C_PANEL2);
    }
    gfx_reset_clip();

    {
        int first = (int)g_app.list_scroll / 46;

        scrollbar_draw(top, view_h, n, view_h / 46 + 1, first);
    }

    hint_begin();
    hint_item("○", active == 1 ? "取消" : "播放/继续");
    hint_item("□", "导出MV");
    hint_item("△", "删除");
    hint_item("L/R", "切换");
    hint_item("×", "返回");
    hint_end();
    (void)t;
}

/* ------------------------------------------------------------------ */
/* 缓存 / 本地音乐                                                     */
/* ------------------------------------------------------------------ */

static void cache_tabs(int y, int tab)
{
    static const char *names[2] = { "缓存", "本地" };
    int x = 14;

    for (int i = 0; i < 2; ++i) {
        int w = gfx_text_w(FS_MD, names[i]) + 20;
        int on = (i == tab);

        if (on) gfx_fill_round(x, y, w, 22, 11, C_ACC);
        else gfx_fill_round(x, y, w, 22, 11, C_PANEL2);
        gfx_text_center(FS_MD, x, w, y + 3, names[i], on ? C_WHITE : C_SUB);
        x += w + 6;
    }
    gfx_text_right(FS_SM, SCR_W - 14, y + 5, "L/R 切换", C_MUTE);
}

static void screen_cache(float t, float dt)
{
    int tab = g_app.cache_tab;
    int top = 64;
    int view_h = content_bottom() - top;
    char right[64];

    panel_bg();

    if (tab == 0) {
        char sz[24];

        fmt_size(sz, sizeof(sz), cache_total_bytes());
        snprintf(right, sizeof(right), "%d 首 · %s", g_cache_total, sz);
        nav_page("缓存", right, 1);
    } else {
        int ln = local_count();
        int free_mb = sysi_ms0_free_mb();

        if (free_mb >= 0) snprintf(right, sizeof(right), "%d 首 · 剩 %dMB", ln, free_mb);
        else snprintf(right, sizeof(right), "%d 首", ln);
        nav_page("本地音乐", right, 1);
    }

    cache_tabs(34, tab);

    if (tab == 0) {
        if (g_app.cache_sel >= g_cache_total && g_cache_total > 0)
            g_app.cache_sel = g_cache_total - 1;
        scroll_follow(&g_app.list_scroll, g_app.cache_sel, 40, view_h, dt);

        if (g_cache_total == 0) {
            gfx_circle_a(SCR_W / 2, 118, 20, C_PANEL2, 255);
            icon_disk(SCR_W / 2, 118, 8, C_MUTE, 220);
            gfx_text_center(FS_MD, 0, SCR_W, 148, "还没有缓存的歌", C_SUB);
            gfx_text_center(FS_SM, 0, SCR_W, 172, "播放过的歌会自动保存到这里", C_MUTE);
        }

        gfx_set_clip(0, top, SCR_W, content_bottom());
        for (int i = 0; i < g_cache_view_n; ++i) {
            const CacheEntry *e = &g_cache_view[i];
            int y = top + (i+g_cache_view_first) * 40 - (int)g_app.list_scroll;
            int sel = (i+g_cache_view_first == g_app.cache_sel);
            int playing = (g_app.has_now && e->id == g_app.now.id);

            if (y < top - 40 || y > content_bottom() + 40) continue;

            if (sel) row_focus(8, y + 2, SCR_W - 16, 36);
            if (playing) icon_bars(22, y + 20, 12, t, C_HOT);
            else icon_disk(22, y + 20, 6, sel ? C_SUB : C_MUTE, 220);

            gfx_text_ellipsis(FS_MD, 42, y + 5, SCR_W - 180, e->name,
                              playing ? C_HOT : C_TXT);
            gfx_text_ellipsis(FS_SM, 42, y + 23, SCR_W - 220, e->artist, C_MUTE);

            {
                char sz[24], dur[16];

                fmt_size(sz, sizeof(sz), e->bytes);
                fmt_time(dur, sizeof(dur), e->dur_ms);
                gfx_text_right(FS_SM, SCR_W - 18, y + 5, sz, C_SUB);
                gfx_text_right(FS_SM, SCR_W - 18, y + 23, dur, C_MUTE);
            }
        }
        gfx_reset_clip();

        if (local_export_active()) {
            int pct = local_export_pct();
            int y = content_bottom() - 22;

            gfx_fill_round(10, y, SCR_W - 20, 18, 9, C_PANEL2);
            gfx_text(FS_SM, 20, y + 3, "正在导出", C_TXT);
            progress_bar(76, y + 8, SCR_W - 176, 3, pct, C_ACC, C_LINE);
            {
                char pb[16];

                snprintf(pb, sizeof(pb), "%d%%", pct);
                gfx_text_right(FS_SM, SCR_W - 20, y + 3, pb, C_ACC);
            }
        }

        {
            int first = (int)g_app.list_scroll / 40;

            scrollbar_draw(top, view_h, g_cache_total, view_h / 40 + 1, first);
        }

        hint_begin();
        hint_item("○", "播放");
        hint_item("△", "导出");
        hint_item("□", "删除");
        hint_item("SELECT", "全删");
        hint_item("×", "返回");
        hint_end();
    } else {
        int ln = local_count();

        if (g_app.local_sel >= ln && ln > 0) g_app.local_sel = ln - 1;
        scroll_follow(&g_app.list_scroll, g_app.local_sel, 34, view_h, dt);

        if (ln == 0) {
            gfx_circle_a(SCR_W / 2, 118, 20, C_PANEL2, 255);
            icon_disk(SCR_W / 2, 118, 8, C_MUTE, 220);
            gfx_text_center(FS_MD, 0, SCR_W, 148, "ms0:/MUSIC 里没有歌", C_SUB);
            gfx_text_center(FS_SM, 0, SCR_W, 172, "把 mp3 放进记忆棒的 MUSIC 文件夹", C_MUTE);
            gfx_text_center(FS_SM, 0, SCR_W, 188, "缓存页按 △ 也能导出到这里", C_MUTE);
        }

        gfx_set_clip(0, top, SCR_W, content_bottom());
        for (int i = 0; i < ln; ++i) {
            int y = top + i * 34 - (int)g_app.list_scroll;
            int sel = (i == g_app.local_sel);
            char sz[24];

            if (y < top - 34 || y > content_bottom() + 34) continue;

            if (sel) row_focus(8, y + 2, SCR_W - 16, 30);
            icon_note(22, y + 17, 6, sel ? C_ACC : C_MUTE, 220);
            gfx_text_ellipsis(FS_MD, 40, y + 7, SCR_W - 160, local_name(i),
                              sel ? C_ACC : C_TXT);
            fmt_size(sz, sizeof(sz), local_size(i));
            gfx_text_right(FS_SM, SCR_W - 18, y + 9, sz, C_MUTE);
        }
        gfx_reset_clip();

        hint_begin();
        hint_item("○", "播放");
        hint_item("SELECT", "重扫");
        hint_item("×", "返回");
        hint_end();
    }
}

/* ------------------------------------------------------------------ */
/* 设置                                                                */
/* ------------------------------------------------------------------ */

#define SET_GROUPS 7
#define SET_ITEMS  MELODY_SETTINGS_COUNT

typedef struct {
    int group;
    const char *label;
} SetDef;

static const SetDef g_set_defs[SET_ITEMS] = {
 {0,"音质"},{0,"音量"},{2,"下载并发"},{4,"皮肤中心"},{2,"网络连接"},{4,"内存显示"},
 {3,"清空下载队列"},{3,"清空缓存"},{5,"账号与登录"},{6,"检查更新"},{6,"关于 MELODY"},
 {2,"备用音源"},{1,"视频解码"},{1,"软解画质"},{1,"视频加载"},{1,"软解格式"},
 {1,"MV清晰度"},{1,"提前缓冲"},{6,"公告"},{5,"隐私政策"},{4,"显示网速"},{0,"预下载后续歌曲"},{4,"界面渲染"},{4,"唱片播放器"},{1,"本地 MV 搜索"},{4,"省电模式"},{4,"自动息屏"},{4,"SoC 时钟"},{6,"更新输入法词库"},{4,"外部超频插件"}
};

/* 把 [top,bottom) 这段保持在可视区里（不是钉在顶部，避免把页头滚没） */
static float follow_visible(float scroll, int top, int bottom, int view_top,
                            int view_bot, int maxscroll, float dt)
{
    int target = (int)scroll;

    if (top - target < view_top) target = top - view_top;
    if (bottom - target > view_bot) target = bottom - view_bot;
    if (target < 0) target = 0;
    if (target > maxscroll) target = maxscroll;
    return approach(scroll, (float)target, 2200.0f, dt);
}

static void settings_value(int i, char *val, int len)
{
    val[0] = 0;
    if(i==29){snprintf(val,len,"%s",overclock_bridge_owns_clock()?"外部接管（非实测MHz）":"未启用（需opicker）");return;}
    if(i==28){if(ime_update_busy())snprintf(val,len,"下载/校验 %d%%",ime_update_percent());else snprintf(val,len,"%.90s",ime_update_message());return;}
    if(i>=25&&i<=27&&power_runtime_locked()){snprintf(val,len,"皮肤接管（锁定）");return;}
    if(i==25){snprintf(val,len,"%s",power_runtime_eco()?"开（空闲30帧）":"关");return;}
    if(i==26){int s=power_runtime_timeout();if(s<0)snprintf(val,len,"常亮");else if(!s)snprintf(val,len,"跟随系统");else snprintf(val,len,"%d秒%s",s,power_runtime_backlight_supported()?"无操作":"（仅画面）");return;}
    if(i==27){int c=power_runtime_clock();if(c)snprintf(val,len,"%d MHz（MV 333）",c);else snprintf(val,len,"自动（当前%d MHz）",power_runtime_effective_clock());return;}
    if(i==24){snprintf(val,len,"%s",store_scan_local_mv()?"开（不推荐·较慢）":"关（只显示下载的 MV）");return;}
    if(i==23){snprintf(val,len,"%s",store_disc_player()?"开":"关");return;}
    /* Show an accepted pending selection immediately; after startup failure,
     * show the actual CPU fallback while retaining the saved preference. */
    if(i==22){const char *err=gfx_renderer_error();int renderer=err&&err[0]?gfx_renderer():store_renderer();snprintf(val,len,"%s",renderer?"GE / GU加速":"CPU软件");return;}
    if(i==21){int count=store_prefetch_count();if(count)snprintf(val,len,"最多后续%d首",count);else snprintf(val,len,"关闭");return;}
    if(i==20){snprintf(val,len,"%s",store_show_speed()?"开":"关");return;}
    if(i==19){snprintf(val,len,"查看文档");return;}
    if(i==18){snprintf(val,len,"%s",community_announcement_get()?"有新公告":"存储桶检查");return;}
    if(i==12){snprintf(val,len,"%s",mv_decoder_mode()==2?"自动":mv_native_requested()?"硬件 AVC（兼容片源）":"软件解码");return;}
    if(i==13){snprintf(val,len,"%s",mv_soft_quality()==1?"流畅（降质）":mv_soft_quality()==2?"均衡":mv_soft_quality()==3?"清晰满屏（较慢）":"原画（精确）");return;}
    if(i==14){snprintf(val,len,"%s",mv_stream_enabled()?"边下边播":"下载后播放");return;}
    if(i==16){int v=mv_resolution();if(v)snprintf(val,len,"%dp（下次下载）",v);else snprintf(val,len,"自动（当前%dp）",mv_download_resolution());return;}
    if(i==17){snprintf(val,len,"约%d秒 / 自动续缓冲",mv_buffer_seconds());return;}
    if(i==15){snprintf(val,len,"MP4 / FLV / AVI / MKV");return;}

    switch (i) {
    case 0:
        snprintf(val, len, "%s", g_app.quality == 0 ? "标准 128k"
                                : (g_app.quality == 1 ? "较高 192k" : "极高 320k"));
        break;
    case 1:
        snprintf(val, len, "%d%%", audio_volume());
        break;
    case 2:
        snprintf(val, len, "%d 路", g_app.dl_parts);
        break;
    case 3:
        snprintf(val,len,"%s",skin_runtime_active()?skin_runtime_style()->name:"默认界面 / 皮肤中心");
        break;
    case 4:
        snprintf(val, len, "%s", g_app.wifi_state == 0
                                ? (g_app.wifi_name[0] ? g_app.wifi_name : "已连接")
                                : "未连接");
        break;
    case 5:
        snprintf(val, len, "%s", g_app.show_mem ? "开" : "关");
        break;
    case 6:
        snprintf(val,len,"%d下载 / %d待解析",dl_queue_count(),api_pending_download_count());
        break;
    case 7: {
        char sz[24];

        fmt_size(sz, sizeof(sz), cache_total_bytes());
        snprintf(val, len, "%s", sz);
        break;
    }
    case 8:
        snprintf(val, len, "%s", api_logged_in() ? api_login_name() : "未登录");
        break;
    case 9: {
        int st = api_update_state();

        if (st == 1) snprintf(val, len, "检查中…");
        else if (st == 2) snprintf(val, len, "已是最新");
        else if (st == 3) snprintf(val, len, "发现 v%s", api_update_version());
        else if(st==7)snprintf(val,len,"等待下载");
        else if (st == 4) snprintf(val, len, "下载 %d%%", api_update_progress());
        else if (st == 5) snprintf(val, len, "已下载，重启生效");
        else if (st == 6) snprintf(val, len, "%s", api_update_message());
        else if (api_update_pending()) snprintf(val, len, "待替换，见安装说明");
        else snprintf(val, len, "v%s", APP_VER);
        break;
    }
    case 10:
        snprintf(val, len, "v%s", APP_VER);
        break;
    case 11: {
        int n = 0;

        for (int i = 0; i < ALT_SOURCE_COUNT; ++i) if (alt_enabled(i)) n++;
        snprintf(val, len, "%d 个已开", n);
        break;
    }
    default:
        break;
    }
}

static int settings_y_of(int sel)
{
    int y = 30;

    for (int i = 0; i < SET_ITEMS; ++i) {
        if (i == 0 || g_set_defs[melody_setting_id(i)].group != g_set_defs[melody_setting_id(i-1)].group) y += 20;
        if (i == sel) return y;
        y += 30;
    }
    return y;
}

static void screen_settings(float t, float dt)
{
    int total_h = settings_y_of(SET_ITEMS) + 8;
    int view_h = content_bottom() - 30;
    int y = 30 - (int)g_app.list_scroll;
    int last_group = -1;
    static const char *group_name[SET_GROUPS] = { "音频", "视频", "网络与下载", "存储管理", "显示", "账号", "社区与关于" };

    panel_bg();
    nav_page("设置", "MELODY " APP_VER, 1);

    {
        int top = settings_y_of(g_app.settings_sel);
        int maxs = total_h - view_h;

        if (maxs < 0) maxs = 0;
        g_app.list_scroll = follow_visible(g_app.list_scroll, top, top + 30, 32,
                                           content_bottom() - 2, maxs, dt);
    }

    gfx_set_clip(0, 30, SCR_W, content_bottom());

    for (int i = 0; i < SET_ITEMS; ++i) {
        char val[48];
        int sel = (g_app.settings_sel == i);
        int ry;

        if (i == 0 || g_set_defs[melody_setting_id(i)].group != last_group) {
            last_group = g_set_defs[melody_setting_id(i)].group;
            gfx_text(FS_SM, 20, y + 6, group_name[last_group], C_MUTE);
            y += 20;
        }

        ry = y;
        if (sel) row_focus(8, ry, SCR_W - 16, 28);

        gfx_text(FS_MD, 20, ry + 6, g_set_defs[melody_setting_id(i)].label, sel ? C_ACC : C_TXT);
        settings_value(melody_setting_id(i), val, sizeof(val));
        if (val[0]) {
            if (melody_setting_id(i) == 8 && api_login_vip() > 0) {
                int vw = gfx_text_w(FS_SM, val);
                int chip_w = gfx_text_w(FS_SM, "VIP") + 8;

                gfx_text_right(FS_SM, SCR_W - 22 - chip_w - 6, ry + 8, val, C_ACC);
                tag_chip(SCR_W - 22 - chip_w, ry + 8, "VIP", C_ACC, 1);
                (void)vw;
            } else {
                gfx_text_right(FS_SM, SCR_W - 22, ry + 8, val, C_MUTE);
            }
        }
        if (i == 0 || i == 2 || i == 3 || i == 4 || i == 8 || i == 9 || i == 10 || i == 11 || i >= 12)
            icon_chevron_right(SCR_W - 16, ry + 9, C_MUTE, 160);
        y += 30;
    }

    gfx_reset_clip();

    {
        int total_rows = total_h / 30 + 1;

        scrollbar_draw(30, view_h, total_rows, view_h / 30 + 1,
                       (int)g_app.list_scroll / 30);
    }

    hint_begin();
    hint_item("↑↓", "选择");
    hint_item("←→", "调整");
    hint_item("○", "确认");
    hint_item("×", "返回");
    hint_end();
    (void)t;
}

/* ------------------------------------------------------------------ */
/* 关于                                                                */
/* ------------------------------------------------------------------ */

static void screen_help(float t, float dt)
{
    (void)t;(void)dt;
    int max_scroll=K_CREDITS_COUNT>9?K_CREDITS_COUNT-9:0;
    if(g_app.help_page<0)g_app.help_page=0;
    if(g_app.help_page>max_scroll)g_app.help_page=max_scroll;
    panel_bg();nav_page("关于 MELODY", "v" APP_VER, 1);
    gfx_set_clip(18,34,SCR_W-36,210);
    for(int i=g_app.help_page;i<K_CREDITS_COUNT;++i){
        int y=39+(i-g_app.help_page)*18;if(y>205)break;
        const char*line=k_credits[i];int heading=!strncmp(line,"## ",3);
        gfx_text(FS_SM,24,y,heading?line+3:line,heading?C_ACC:C_TXT);
    }
    gfx_reset_clip();
    scrollbar_draw(35,170,K_CREDITS_COUNT,9,g_app.help_page);
    hint_begin();hint_item("↑↓ / 摇杆","滚动");hint_item("L/R","翻页");hint_item("×","返回");hint_end();
}

/* ------------------------------------------------------------------ */
/* 账号 / 扫码登录                                                     */
/* ------------------------------------------------------------------ */

static void screen_login(float t, float dt)
{
    char right[48];
    int dim = 0;
    const unsigned char *mods;

    (void)t;
    (void)dt;
    panel_bg();

    right[0] = 0;
    if (api_logged_in()) snprintf(right, sizeof(right), "%s", api_login_name());
    nav_page("账号", right, 1);

    mods = api_qr_matrix(&dim);

    if (api_logged_in()) {
        gfx_fill_round(12, 40, SCR_W - 24, 72, 8, C_PANEL2);
        {
            int slot = api_avatar_slot(api_user_avatar());
            const unsigned short *px = api_avatar_px(slot);

            if (px) gfx_blit565_round(24, 52, 48, 48, px, 48, 48, 24, 255);
            else {
                gfx_circle(48, 76, 24, C_ACC);
                if (api_login_name()[0]) {
                    char one[8];

                    first_char(api_login_name(), one, sizeof(one));
                    gfx_text_center(FS_LG, 24, 48, 64, one, C_WHITE);
                }
            }
        }
        gfx_text(FS_MD, 84, 52, api_login_name()[0] ? api_login_name() : "已登录", C_TXT);
        {
            char b[48];

            snprintf(b, sizeof(b), "UID %lld", api_login_uid());
            gfx_text(FS_SM, 84, 76, b, C_MUTE);
        }
        if (api_login_vip() > 0) tag_chip(84, 90, "VIP 会员", C_ACC, 1);
        else gfx_text(FS_SM, 84, 91, "普通账号", C_MUTE);

        gfx_text(FS_SM, 20, 128, "登录后：320k 音质、账号有权限的 VIP 歌曲、云端喜欢同步",
                 C_SUB);
        gfx_text(FS_SM, 20, 146, "按 △ 退出登录（会删除 cookie.txt）", C_MUTE);

        {
            int bx = 20, by = 176, bw = SCR_W - 40;

            gfx_fill_round(bx, by, bw, 40, 6, C_PANEL2);
            gfx_text(FS_SM, bx + 12, by + 6, "登录状态", C_MUTE);
            gfx_text(FS_MD, bx + 12, by + 20, "已登录 · 凭据已保存", C_OK);
        }
    } else {
        /* 二维码卡片 */
        if (mods && dim > 0) {
            int cell = (dim <= 37) ? 4 : 3;
            int size = dim * cell;
            int px = 18, py = 40;

            gfx_fill_round(px - 6, py - 6, size + 12, size + 12, 4, C_WHITE);
            gfx_round_border(px - 6, py - 6, size + 12, size + 12, 4, C_LINE);
            for (int y = 0; y < dim; ++y)
                for (int x = 0; x < dim; ++x)
                    if (mods[y * dim + x])
                        gfx_fill_rect(px + x * cell, py + y * cell, cell, cell, C_BLACK);
        } else {
            gfx_fill_round(18, 40, 150, 150, 4, C_PANEL2);
            gfx_text_center(FS_SM, 18, 150, 110, "正在获取二维码...", C_MUTE);
        }

        {
            int x = 190;
            const char *state;
            int sc = api_qr_state();

            gfx_text(FS_MD, x, 44, "手机扫码登录", C_TXT);
            gfx_text(FS_SM, x, 72, "1. 打开手机网易云音乐", C_SUB);
            gfx_text(FS_SM, x, 90, "2. 右上角「扫一扫」", C_SUB);
            gfx_text(FS_SM, x, 108, "3. 确认登录", C_SUB);

            switch (sc) {
            case 801: state = "等待扫码"; break;
            case 802: state = "已扫描，请在手机上确认"; break;
            case 803: state = "登录成功"; break;
            case 800: state = "二维码已过期"; break;
            case -1: state = "网络失败"; break;
            case 8821: state = "被网易风控拦了（8821）"; break;
            default:
                state = (sc > 1000) ? "登录被拒绝" : "正在获取二维码 ...";
                break;
            }

            gfx_text(FS_SM, x, 140, state, sc == 803 ? C_OK : C_ACC);
            if (sc == 800 || sc == -1 || sc == 8821)
                gfx_text(FS_SM, x, 158, "按 ○ 重新获取", C_MUTE);

            gfx_text(FS_SM, x, 186, "扫不了可以导入 cookie.txt", C_MUTE);
        }
    }

    hint_begin();
    if (api_logged_in()) {
        hint_item("△", "退出登录");
        hint_item("×", "返回");
    } else {
        hint_item("○", "刷新二维码");
        hint_item("△", "重新获取");
        hint_item("×", "返回");
    }
    hint_end();
}

/* ==== PART5 ==== */

/* ------------------------------------------------------------------ */
/* 封面网格（推荐歌单 / 我的歌单 / 排行榜 / 歌手榜）                   */
/* ------------------------------------------------------------------ */

static void screen_grid(float t, float dt)
{
    const CardList *cl = api_cards();
    int pages = (cl->count + 9) / 10;
    char right[48];

    (void)t;
    (void)dt;
    panel_bg();

    if (pages < 1) pages = 1;
    snprintf(right, sizeof(right), "%d/%d", g_app.grid_page + 1, pages);
    nav_page(cl->title[0] ? cl->title : "歌单", right, 1);

    if (cl->count == 0) {
        gfx_text_center(FS_MD, 0, SCR_W, 108, "没有内容", C_SUB);
        gfx_text_center(FS_SM, 0, SCR_W, 132, "按 ○ 重新加载", C_ACC);
    }

    for (int i = 0; i < 10; ++i) {
        int idx = g_app.grid_page * 10 + i;
        int col = i % 5, row = i / 5;
        int x = 34 + col * 84, y = 32 + row * 92;
        int sel = (g_app.grid_sel == i);
        const Card *k;

        if (idx >= cl->count) break;
        k = &cl->items[idx];

        if (sel) gfx_round_border(x - 2, y - 2, UI_COVER_SZ + 4, UI_COVER_SZ + 4, 6, C_ACC);
        draw_cover(x, y, UI_COVER_SZ, 4, thumb_of(k->cover), 112, 112);
        if(store_playlist_has(k->id))tag_chip(x+2,y+2,"已收藏",C_ACC,1);

        {
            char b[24];

            fmt_count(b, sizeof(b), k->plays);
            if (b[0]) {
                int cw = gfx_text_w(FS_SM, b);

                gfx_fill_round_a(x + 4, y + UI_COVER_SZ - 18, UI_COVER_SZ - 8, 14, 3,
                                 C_BLACK, 115);
                icon_headset(x + UI_COVER_SZ - cw - 12, y + UI_COVER_SZ - 11, C_WHITE, 210);
                gfx_text(FS_SM, x + UI_COVER_SZ - cw - 5, y + UI_COVER_SZ - 17,
                         b, C_WHITE);
            }
        }

        gfx_text_ellipsis(FS_SM, x, y + UI_COVER_SZ + 4, UI_COVER_SZ, k->name,
                          sel ? C_ACC : C_TXT);
    }

    api_request_thumbs();

    hint_begin();
    hint_item("○", "打开");
    hint_item("△", "收藏歌单");
    hint_item("□", "播放全部");
    hint_item("L/R", "翻页");
    hint_item("×", "返回");
    hint_end();
}

/* ------------------------------------------------------------------ */
/* 歌单广场（分类 + 封面）                                              */
/* ------------------------------------------------------------------ */

static const char *g_sq_cat[8] = { "华语", "流行", "摇滚", "电子",
                                   "民谣", "说唱", "轻音乐", "古典" };

static void screen_square(float t, float dt)
{
    const CardList *cl=api_discovery_cards();
    int pages = (cl->count + 11) / 12;
    char right[48];

    (void)t;
    (void)dt;
    panel_bg();

    if (pages < 1) pages = 1;
    snprintf(right, sizeof(right), "%d/%d", g_app.grid_page + 1, pages);
    nav_page("歌单广场", right, 1);

    for (int i = 0; i < 8; ++i) {
        int x = 17 + i * 56, y = 32, w = 54;
        int sel = (g_app.cat_sel == i && g_app.cat_focus == 0);

        gfx_fill_round(x, y, w, 20, 10, sel ? C_ACC : C_PANEL2);
        if (!sel) gfx_round_border_a(x, y, w, 20, 10, C_LINE, 150);
        gfx_text_center(FS_SM, x, w, y + 4, g_sq_cat[i], sel ? C_WHITE : C_SUB);
    }

    for (int i = 0; i < 12; ++i) {
        int idx = g_app.grid_page * 12 + i;
        int col = i % 6, row = i / 6;
        int x = 28 + col * 72, y = 56 + row * 80;
        int sel = (g_app.grid_sel == i && g_app.cat_focus == 1);
        const Card *k;

        if (idx >= cl->count) break;
        k = &cl->items[idx];

        if (sel) gfx_round_border(x - 2, y - 2, 68, 68, 6, C_ACC);
        draw_cover(x, y, 64, 4, thumb_of(k->cover), 112, 112);
        gfx_text_ellipsis(FS_SM, x, y + 67, 64, k->name, sel ? C_ACC : C_TXT);
    }

    if (cl->count == 0) gfx_text_center(FS_SM, 0, SCR_W, 140, "加载中 ...", C_MUTE);

    api_request_thumbs();

    hint_begin();
    hint_item("○", "打开");
    hint_item("△", "收藏歌单");
    if (g_app.cat_focus == 0) hint_item("←→", "分类");
    else hint_item("↑", "分类");
    hint_item("L/R", "翻页");
    hint_item("×", "返回");
    hint_end();
}

/* ------------------------------------------------------------------ */
/* 评论                                                                */
/* ------------------------------------------------------------------ */

/* 按宽度折行画，返回用掉的行数 */
static int text_wrap(int x, int y, int maxw, const char *s, unsigned short color,
                     int maxlines, int lh)
{
    int line = 0, cx = 0;
    const char *p = s;

    while (*p && line < maxlines) {
        int n = 1;

        while ((p[n] & 0xC0) == 0x80) n++;

        if (cx + gfx_text_w_n(FS_SM, p, n) > maxw) {
            line++;
            if (line >= maxlines) break;
            y += lh;
            cx = 0;
            continue;
        }
        gfx_text_n(FS_SM, x + cx, y, n, p, color);
        cx += gfx_text_w_n(FS_SM, p, n);
        p += n;
    }
    return line + 1;
}

static int comment_draw_measure(const char *s, int n) { return gfx_text_w_n(FS_SM, s, n); }
typedef struct { int x, y; unsigned int color; } CommentDrawContext;
static void comment_draw_glyph(int x, int y, const char *s, int n, void *ud)
{
    CommentDrawContext *d = (CommentDrawContext *)ud;
    gfx_text_n(FS_SM, d->x + x, d->y + y, n, s, d->color);
}
static void comment_draw_body(const Comment *c, int x, int y, int width, int selected)
{
    CommentDrawContext d = { x, y + 18, C_TXT };
    char footer[80];
    int h = comment_body_height(c, width, comment_draw_measure);
    gfx_text_ellipsis(FS_SM, x, y + 1, width - 90, c->user, selected ? C_ACC : C_LINK);
    if (c->hot) gfx_text_right(FS_SM, x + width, y + 1, "热门", C_ACC);
    comment_wrap(c->text, width, comment_draw_measure, comment_draw_glyph, &d);
    if(c->reply_count>=0)snprintf(footer,sizeof(footer),"赞 %d   回复 %d",c->liked,c->reply_count);
    else snprintf(footer,sizeof(footer),"赞 %d   回复%s",c->liked,c->reply_count==-2?"加载中":"（展开查看）");
    gfx_text(FS_SM, x, y + h - 16, footer, C_MUTE);
    if (c->ip[0]) gfx_text_right(FS_SM, x + width, y + h - 16, c->ip, C_MUTE);
}
/* Paint once, after the scrolling list: even a projected texture that escapes
 * the list scissor must be covered across the entire status/header area. */
static void comment_header_draw(int metro, int top, int loaded)
{
    char right[64];
    gfx_set_clip(0, 0, SCR_W, top);
    gfx_fill_rect(0, 0, SCR_W, top, C_PANEL);
    if(loaded>=MAX_COMMENTS)snprintf(right,sizeof(right),"已加载 %d 条 · 缓存上限",loaded);
    else snprintf(right,sizeof(right),"已加载 %d / %d 条",loaded,api_comment_total());
    if (metro) {
        gfx_text(FS_SM,16,6,"MELODY",C_MUTE);
        gfx_text(FS_LG,15,23,"评论",C_TXT);
        gfx_text_ellipsis(FS_SM,17,55,260,api_comment_name(),C_SUB);
        gfx_text_right(FS_SM,SCR_W-17,55,right,C_MUTE);
    } else {
        nav_page("评论",right,1);
        gfx_text_ellipsis(FS_SM,12,34,SCR_W-24,api_comment_name(),C_SUB);
    }
    gfx_reset_clip();
}
static void screen_comments(float t, float dt)
{
    const CommentList *cm = api_comments();
    CommentView *v = api_comment_view();
    const CommentReplies *r;
    int metro=is_metro_style();
    int top=metro?76:52,width=SCR_W-70,y,total;
    (void)t; (void)dt;
    if (v->generation != api_comment_generation() || v->page != api_comment_page()) {
        v->generation = api_comment_generation(); v->page = api_comment_page();
        v->selected = 0; v->expanded = 0; g_app.cm_scroll = 0;
    }
    r = api_comment_replies(v->expanded);
    panel_bg();
    v->viewport = content_bottom() - top;
    total = comment_list_height(cm, v, r, width, comment_draw_measure);
    g_app.cm_scroll = comment_scroll_clamp(g_app.cm_scroll, total, content_bottom() - top);
    /* A scope keeps nested drawing from resetting the list's bounds. */
    if (gfx_push_clip(0, top, SCR_W, content_bottom()) == 0) {
    y = top - (int)g_app.cm_scroll;
    if (!cm->count) gfx_text_center(FS_SM, 0, SCR_W, 126, "还没有评论，或者正在加载", C_MUTE);
    for (int i = 0; i < cm->count; ++i) {
        const Comment *c = &cm->items[i];
        int h = comment_row_height(c, v->expanded, r, width, comment_draw_measure);
        int body = comment_body_height(c, width, comment_draw_measure);
        if (y >= content_bottom()) break;
        if (y + h > top) {
            api_request_comment_count(c->id);
            int av = api_avatar_slot(c->avatar);
            const unsigned short *px = api_avatar_px(av);
            if(px)gfx_blit565_round(14,y+2,30,30,px,48,48,metro?0:15,255);
            else if(metro){gfx_fill_rect(14,y+2,30,30,C_ACC);gfx_text(FS_MD,23,y+9,"人",C_WHITE);}else gfx_circle(29,y+17,15,C_PANEL2);
            if(i==v->selected){if(metro)gfx_fill_rect(5,y+2,3,h-5,C_ACC);else gfx_text(FS_SM,2,y+2,">",C_ACC);}
            comment_draw_body(c, 52, y, width, i == v->selected);
            if (c->id && c->id == v->expanded) {
                int ry = y + body;
                const char *status = api_comment_replies_loading(c->id) ? "回复加载中..." :
                    (api_comment_replies_failed(c->id) ? "加载失败，○关闭后重试" :
                     (!r->count ? "暂无回复" : (r->more ? "楼层回复（显示前32条）" : "楼层回复")));
                if(metro)gfx_fill_rect(56,ry,2,h-body-3,C_ACC);
                gfx_text(FS_SM,68,ry,status,metro?C_ACC:C_SUB);ry+=20;
                for (int j = 0; j < r->count; ++j) {
                    int rh = comment_body_height(&r->items[j], width - 16, comment_draw_measure);
                    if (ry < content_bottom() && ry + rh > top)
                        comment_draw_body(&r->items[j], 68, ry, width - 16, 0);
                    ry += rh;
                }
            }
            gfx_hline(14, y + h - 3, SCR_W - 28, C_LINE);
        }
        y += h;
    }
    gfx_pop_clip();
    }
    gfx_reset_clip();
    comment_header_draw(metro, top, cm->count);
    api_request_thumbs();
    hint_begin();
    hint_item("↑↓", "选择/滚动"); hint_item("←→", "整屏快滚");
    hint_item("○", "展开/收起"); hint_item("×", "返回");
    hint_end();
}

/* ------------------------------------------------------------------ */
/* 我的                                                                */
/* ------------------------------------------------------------------ */

#define MINE_ITEMS 12

static const char *g_mine_label[MINE_ITEMS] = {
    "我喜欢的音乐", "最近播放", "我的歌单", "本地音乐",
    "下载管理", "缓存管理", "设置", "账号与登录",
    "歌单广场", "歌手榜", "关于 MELODY", "收藏的歌单"
};

static int mine_y_of(int sel)
{
    int y = 34 + 64 + 8;

    for (int i = 0; i < MINE_ITEMS; ++i) {
        if (i == 0 || i == 4 || i == 8) y += 20;
        if (i == sel) return y;
        y += 30;
    }
    return y;
}

static void mine_icon(int i, int cx, int cy, unsigned short c)
{
    switch (i) {
    case 0: icon_heart(cx, cy, 7, c, 230); break;
    case 1: icon_bars(cx, cy, 10, 1.2f, c); break;
    case 2: icon_list(cx, cy, 6, c, 230); break;
    case 3: icon_disk(cx, cy, 6, c, 230); break;
    case 4: icon_download(cx, cy, 5, c, 230); break;
    case 5: icon_disk(cx, cy, 6, c, 230); break;
    case 6: icon_gear(cx, cy, 6, c, 230); break;
    case 7: icon_person(cx, cy, 6, c, 230); break;
    case 8: icon_list(cx, cy, 6, c, 230); break;
    case 9: icon_person(cx, cy, 6, c, 230); break;
    default: gfx_text_center(FS_SM, cx - 8, 16, cy - 6, "i", c); break;
    }
}

static void screen_mine(float t, float dt)
{
    int total_h = mine_y_of(MINE_ITEMS) + 10;
    int view_h = content_bottom() - 30;
    int y = 34 + 64 + 8 - (int)g_app.mine_scroll;
    static const char *group[3] = { "我的音乐", "管理与工具", "发现" };
    int last = -1;

    (void)t;
    panel_bg();
    nav_tabs();

    {
        int top = mine_y_of(g_app.mine_sel);
        int maxs = total_h - view_h;

        if (maxs < 0) maxs = 0;
        g_app.mine_scroll = follow_visible(g_app.mine_scroll, top, top + 30, 32,
                                           content_bottom() - 2, maxs, dt);
    }

    gfx_set_clip(0, 30, SCR_W, content_bottom());

    /* 账号卡 */
    {
        int py = 34 - (int)g_app.mine_scroll;

        gfx_fill_round(12, py, SCR_W - 24, 64, 8, C_PANEL2);
        {
            int slot = api_avatar_slot(api_user_avatar());
            const unsigned short *px = api_avatar_px(slot);

            if (px) gfx_blit565_round(24, py + 12, 40, 40, px, 48, 48, 20, 255);
            else {
                gfx_circle(44, py + 32, 20, C_ACC);
                if (api_login_name()[0]) {
                    char one[8];

                    first_char(api_login_name(), one, sizeof(one));
                    gfx_text_center(FS_MD, 24, 40, py + 23, one, C_WHITE);
                } else {
                    icon_person(44, py + 33, 7, C_WHITE, 230);
                }
            }
        }
        gfx_text(FS_MD, 76, py + 12, api_login_name()[0] ? api_login_name() : "未登录",
                 C_TXT);
        if (api_login_vip() > 0) {
            tag_chip(76, py + 38, "VIP 会员", C_ACC, 1);
        } else {
            gfx_text(FS_SM, 76, py + 40,
                     api_logged_in() ? "普通账号" : "登录后可同步喜欢与歌单", C_MUTE);
        }
        { int lv = g_app.user_level; char b[40];
          snprintf(b, sizeof(b), "Lv.%d", lv > 0 ? lv : 1);
          gfx_text_right(FS_SM, SCR_W - 24, py + 13, b, C_MUTE); }
        gfx_text_right(FS_SM, SCR_W - 24, py + 40,
                       api_logged_in() ? "△ 账号" : "△ 去登录", C_MUTE);
    }

    for (int i = 0; i < MINE_ITEMS; ++i) {
        int sel = (g_app.mine_sel == i);
        int ry;
        char val[32] = "";

        if (i == 0 || i == 4 || i == 8) {
            last = (i == 0) ? 0 : (i == 4 ? 1 : 2);
            gfx_text(FS_SM, 20, y + 4, group[last], C_MUTE);
            y += 20;
        }

        ry = y;
        if (sel) row_focus(8, ry, SCR_W - 16, 28);

        mine_icon(i, 26, ry + 14, sel ? C_ACC : C_SUB);
        gfx_text(FS_MD, 44, ry + 6, g_mine_label[i], sel ? C_ACC : C_TXT);

        switch (i) {
        case 0: {
            /* 爱心绑定的是云端「我喜欢的音乐」，加载过就显示云端曲目数 */
            int n = api_liked_total();

            if (n <= 0) n = store_fav_count();
            snprintf(val, sizeof(val), "%d 首", n);
            break;
        }
        case 1: snprintf(val, sizeof(val), "%d 首", store_recent_count()); break;
        case 3: snprintf(val, sizeof(val), "%d 首", local_count()); break;
        case 4: snprintf(val, sizeof(val), "%d 个", dl_queue_count()); break;
        case 5: snprintf(val, sizeof(val), "%d 首", cache_count()); break;
        default: val[0] = 0; break;
        }
        if (val[0]) gfx_text_right(FS_SM, SCR_W - 34, ry + 8, val, C_MUTE);
        icon_chevron_right(SCR_W - 16, ry + 9, C_MUTE, 150);
        y += 30;
    }

    gfx_reset_clip();

    {
        int total_rows = total_h / 30 + 1;

        scrollbar_draw(30, view_h, total_rows, view_h / 30 + 1,
                       (int)g_app.mine_scroll / 30);
    }

    api_request_thumbs();

    hint_begin();
    hint_item("○", "打开");
    hint_item("L/R", "切换");
    hint_item("×", "返回");
    hint_end();
}

/* ------------------------------------------------------------------ */
/* 网络连接                                                            */
/* ------------------------------------------------------------------ */

static const char *wifi_secure_name(int s)
{
    if (s == 0) return "开放";
    if (s == 1) return "WEP";
    if (s == 2) return "WPA";
    return "--";
}

static void screen_wifi_metro(float t, float dt)
{
    int bottom=content_bottom();
    gfx_clear(metro_background());
    gfx_fill_rect(0,0,SCR_W,3,C_ACC);
    gfx_text(FS_LG,18,12,"Wi-Fi",C_TXT);
    gfx_text_right(FS_SM,SCR_W-18,23,g_app.wlan_on?"WLAN 已开启":"WLAN 已关闭",g_app.wlan_on?C_ACC:C_WARN);
    gfx_text(FS_SM,18,48,g_app.wlan_on?"已保存的网络连接":"请打开机身侧面的 WLAN 开关",C_SUB);
    int visible=(bottom-86)/36;if(visible<1)visible=1;
    int first=g_app.profile_sel-visible+1;if(first<0)first=0;
    if(!g_app.profile_count){
        gfx_text(FS_MD,18,88,"没有已保存的连接",C_TXT);
        gfx_text(FS_SM,18,116,"按 △ 打开系统网络设置",C_SUB);
        gfx_text(FS_SM,18,138,"PSP 使用 WEP / WPA-PSK (TKIP)",C_MUTE);
    }
    for(int i=first;i<g_app.profile_count&&i<first+visible;++i){
        const Profile*p=&g_app.profiles[i];int y=72+(i-first)*36;
        int selected=i==g_app.profile_sel;
        if(selected){gfx_fill_rect(12,y,SCR_W-24,34,C_PANEL2);gfx_fill_rect(12,y,3,34,C_ACC);}
        gfx_text_ellipsis(FS_MD,24,y+2,300,p->name[0]?p->name:"未命名连接",selected?C_ACC:C_TXT);
        gfx_text_ellipsis(FS_SM,24,y+20,300,p->ssid[0]?p->ssid:"（无 SSID）",C_SUB);
        gfx_text_right(FS_SM,SCR_W-24,y+11,wifi_secure_name(p->secure),C_MUTE);
    }
    if(g_app.connecting){
        gfx_fill_rect(12,bottom-20,SCR_W-24,20,metro_background());
        gfx_text_ellipsis(FS_SM,18,bottom-17,SCR_W-36,g_app.connect_msg,
                          g_app.connecting==3?C_WARN:C_ACC);
        if(g_app.connecting==1)spinner_draw(SCR_W-22,bottom-10,t);
    }
    gfx_fill_rect(0,hint_y(),SCR_W,UI_HINT_H,C_PANEL2);
    gfx_text(FS_SM,12,hint_y()+1,"○ 连接   △ 系统设置   SELECT 刷新   × 返回",C_TXT);
    (void)dt;
}

static void screen_wifi(float t, float dt)
{
    if(is_metro_style()){screen_wifi_metro(t,dt);return;}
    panel_bg();
    nav_page("网络连接", g_app.wlan_on ? "WLAN 开" : "WLAN 关", 1);

    {
        int on = g_app.wlan_on;
        int bx = 12, by = 34, bw = SCR_W - 24, bh = 38;

        gfx_fill_round(bx, by, bw, bh, 8, on ? gfx_lerp565(C_OK, C_PANEL, 220)
                                            : gfx_lerp565(C_WARN, C_PANEL, 220));
        gfx_round_border_a(bx, by, bw, bh, 8, on ? C_OK : C_WARN, 130);
        gfx_circle(bx + 18, by + bh / 2, 5, on ? C_OK : C_WARN);
        gfx_text(FS_MD, bx + 32, by + 10, on ? "WLAN 已打开" : "WLAN 是关的",
                 on ? C_TXT : C_WARN);
        gfx_text_right(FS_SM, bx + bw - 12, by + 14,
                       on ? "网络连接正常" : "把机身侧面的拨钮拨到 ON",
                       on ? C_MUTE : C_WARN);
    }

    if (g_app.profile_count == 0) {
        gfx_circle_a(SCR_W / 2, 120, 20, C_PANEL2, 255);
        icon_wifi(SCR_W / 2, 126, 0);
        gfx_text_center(FS_MD, 0, SCR_W, 150, "没有已保存的网络连接", C_SUB);
        gfx_text_center(FS_SM, 0, SCR_W, 174, "按 △ 打开系统网络设置新建一个", C_MUTE);
        gfx_text_center(FS_SM, 0, SCR_W, 190, "PSP-1000 只认 WEP / WPA-PSK(TKIP)", C_MUTE);
    } else {
        for (int i = 0; i < g_app.profile_count; ++i) {
            const Profile *p = &g_app.profiles[i];
            int sel = (i == g_app.profile_sel);
            int ry = 82 + i * 40;

            if (ry > content_bottom() - 40) break;
            if (sel) row_focus(8, ry, SCR_W - 16, 36);

            gfx_round_border_a(26, ry + 10, 16, 14, 2, sel ? C_ACC : C_MUTE, 200);
            gfx_fill_rect(30, ry + 6, 8, 4, sel ? C_ACC : C_MUTE);

            gfx_text_ellipsis(FS_MD, 54, ry + 4, 280,
                              p->name[0] ? p->name : "未命名连接", C_TXT);
            gfx_text_ellipsis(FS_SM, 54, ry + 22, 280,
                              p->ssid[0] ? p->ssid : "（无 SSID）", C_MUTE);

            {
                const char *tag = wifi_secure_name(p->secure);
                int tw = gfx_text_w(FS_SM, tag) + 14;
                unsigned short tc = p->secure == 2 ? C_OK
                                    : (p->secure == 1 ? C_WARN : C_MUTE);

                gfx_fill_round_a(SCR_W - 28 - tw, ry + 10, tw, 16, 4, tc, 36);
                gfx_text_center(FS_SM, SCR_W - 28 - tw, tw, ry + 11, tag, tc);
            }
        }
    }

    if (g_app.connecting == 1) {
        int w = 280, h = 78, x = (SCR_W - w) / 2, y = 108;
        int dots = (int)(t * 6) % 4;

        gfx_fill_round_a(x - 2, y - 2, w + 4, h + 4, 12, C_BLACK, 40);
        gfx_fill_round(x, y, w, h, 10, C_PANEL);
        gfx_round_border(x, y, w, h, 10, C_LINE);
        gfx_text_center(FS_MD, x, w, y + 14, "正在连接 ...", C_TXT);
        gfx_text_center(FS_SM, x, w, y + 40, g_app.connect_msg, C_SUB);
        for (int i = 0; i < 3; ++i)
            gfx_circle_a(x + w / 2 - 12 + i * 12, y + h - 14, 2,
                         i < dots ? C_ACC : C_PANEL2, 255);
    } else if (g_app.connecting == 3) {
        gfx_text_center(FS_SM, 0, SCR_W, content_bottom() - 16, g_app.connect_msg, C_WARN);
    } else if (g_app.connecting == 2) {
        gfx_text_center(FS_SM, 0, SCR_W, content_bottom() - 16, g_app.connect_msg, C_OK);
    }

    hint_begin();
    hint_item("○", "连接");
    hint_item("△", "系统设置");
    hint_item("SELECT", "刷新");
    hint_item("×", "返回");
    hint_end();
    (void)dt;
}

/* ------------------------------------------------------------------ */
/* 总入口                                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 备用音源                                                            */
/* ------------------------------------------------------------------ */

static void screen_alt(float t, float dt)
{
    (void)t;
    (void)dt;
    panel_bg();
    nav_page("备用音源", "VIP 回退", 1);

    gfx_text(FS_SM, 20, 34, "网易只给 45 秒试听 / 不给地址时，按顺序试这些源", C_MUTE);

    for (int i = 0; i < ALT_SOURCE_COUNT; ++i) {
        int y = 54 + i * 40;
        int sel = (g_app.alt_sel == i);
        int on = alt_enabled(i);
        const char *desc = alt_desc(i);

        if (sel) row_focus(8, y, SCR_W - 16, 36);
        gfx_text(FS_MD, 20, y + 2, alt_name(i), sel ? C_ACC : C_TXT);
        if (i == ALT_GATEWAY && alt_gateway()[0]) desc = alt_gateway();
        gfx_text_ellipsis(FS_SM, 20, y + 21, SCR_W - 120, desc, C_SUB);
        tag_chip(SCR_W - 70, y + 8, on ? "开启" : "关闭", on ? C_OK : C_MUTE, on);
    }

    hint_begin();
    hint_item("↑↓", "选择");
    hint_item("○", "开关");
    hint_item("×", "返回");
    hint_end();
}

static SceUID g_skin_preview_block=-1;
static unsigned char*g_skin_preview_memory;
void ui_free_skin_preview(void){if(g_skin_preview_block>=0)sceKernelFreePartitionMemory(g_skin_preview_block);g_skin_preview_block=-1;g_skin_preview_memory=NULL;}
static int skin_preview_allocate(void){if(g_skin_preview_memory)return 1;g_skin_preview_block=sceKernelAllocPartitionMemory(2,"melody_skin_preview",PSP_SMEM_High,448*1024,NULL);if(g_skin_preview_block<0)return 0;g_skin_preview_memory=sceKernelGetBlockHeadAddr(g_skin_preview_block);if(!g_skin_preview_memory){ui_free_skin_preview();return 0;}return 1;}
static void screen_skins(float time,float dt){
    static int refresh_preview_after_work;
    static char row_ids[SKIN_CATALOG_MAX][33],row_versions[SKIN_CATALOG_MAX][24];
    if(api_skin_busy())memset(row_ids,0,sizeof(row_ids));
    if(api_skin_busy())refresh_preview_after_work=1;
    (void)time;panel_bg();char active_skin[96];const SkinStyle*current=skin_runtime_style();snprintf(active_skin,sizeof(active_skin),"%s %s",skin_runtime_active()?current->id:"系统默认",skin_runtime_active()?current->version:"");nav_page("皮肤下载中心",active_skin,1);
    if(api_skin_busy()){char work[96];snprintf(work,sizeof(work),"皮肤请求处理中 %d%%",skin_catalog_progress());gfx_text(FS_SM,18,40,work,C_ACC);}
    const SkinCatalogEntry*items=skin_catalog_items();int count=api_skin_busy()?0:skin_catalog_count();
    if(!api_skin_busy()&&g_app.skin_sel>count)g_app.skin_sel=count;
    int top=62,row=38,view=content_bottom()-top;scroll_follow(&g_app.skin_scroll,g_app.skin_sel,row,view,dt);
    gfx_set_clip(8,top,238,content_bottom());for(int i=0;i<=count;++i){int y=top+i*row-(int)g_app.skin_scroll;if(y<top-row||y>=content_bottom())continue;if(i==g_app.skin_sel)row_focus(10,y,225,36);gfx_text_ellipsis(FS_MD,18,y+3,211,i?items[i-1].name:"系统默认",i==g_app.skin_sel?C_ACC:C_TXT);if(i){char row_detail[96],installed_version[24]={0};if(items[i-1].installed){int slot=i-1;if(strcmp(row_ids[slot],items[slot].id)){snprintf(row_ids[slot],sizeof(row_ids[slot]),"%s",items[slot].id);row_versions[slot][0]=0;skin_catalog_local_version(items[slot].id,row_versions[slot],sizeof(row_versions[slot]));}snprintf(installed_version,sizeof(installed_version),"%s",row_versions[slot]);}snprintf(row_detail,sizeof(row_detail),"%s · %s",installed_version[0]?installed_version:items[i-1].version,items[i-1].installed?"○应用":"○安装");gfx_text_ellipsis(FS_SM,18,y+23,211,row_detail,C_MUTE);}else gfx_text(FS_SM,18,y+23,"恢复原始界面",C_MUTE);}gfx_reset_clip();
    if(skin_catalog_install_error()[0])gfx_text_ellipsis(FS_SM,245,43,230,skin_catalog_install_error(),C_WARN);
    else gfx_text_ellipsis(FS_SM,245,43,230,"SELECT · 当前主题设置 / 颜色",C_MUTE);
    if(g_app.skin_sel>0&&g_app.skin_sel<=count){const SkinCatalogEntry*entry=&items[g_app.skin_sel-1];
        static char shown[33],local_version[24];unsigned char*rgba=NULL;unsigned short*pixels=NULL;static int width,height;static int okay;
        if(!g_skin_preview_memory)okay=0;
        if(skin_preview_allocate()){rgba=g_skin_preview_memory+SKIN_CATALOG_PREVIEW_MAX;pixels=(unsigned short*)(rgba+224*224*4);}
        if(refresh_preview_after_work){okay=0;shown[0]=0;refresh_preview_after_work=0;}
        if(strcmp(shown,entry->id)){snprintf(shown,sizeof(shown),"%s",entry->id);okay=0;local_version[0]=0;skin_catalog_local_version(entry->id,local_version,sizeof(local_version));}
        if(!okay){char path[256];if(skin_catalog_preview_path(entry->id,path,sizeof(path))==0){FILE*f=fopen(path,"rb");if(f){unsigned char*bytes=g_skin_preview_memory;int n=bytes?fread(bytes,1,SKIN_CATALOG_PREVIEW_MAX,f):0;fclose(f);int decoded=n>0&&rgba&&pixels?png_decode_rgba_scaled(bytes,n,rgba,224,&width,&height):-1;if(decoded==0){for(int k=0;k<width*height;++k)pixels[k]=gfx_rgb(rgba[k*4],rgba[k*4+1],rgba[k*4+2]);okay=1;mlog("skin preview: ready id=%s size=%dx%d",entry->id,width,height);}else mlog("skin preview: decode failed id=%s bytes=%d rgba=%d pixels=%d",entry->id,n,rgba!=NULL,pixels!=NULL);}}}
        if(okay&&pixels){int draw_h=height;if(draw_h>126)draw_h=126;gfx_set_clip(245,64,475,190);gfx_blit565(245,64,width,draw_h,pixels,width);gfx_reset_clip();}else gfx_text_center(FS_SM,245,225,118,"□ 获取预览",C_MUTE);
        gfx_text_ellipsis(FS_SM,246,191,224,entry->author,C_SUB);char version[64];snprintf(version,sizeof(version),"本地%s / 目录%s",local_version[0]?local_version:"未装",entry->version);gfx_text(FS_SM,246,207,version,C_MUTE);gfx_text_ellipsis(FS_SM,246,224,224,entry->description,C_SUB);
    }else if(!api_skin_busy()&&!count)gfx_text_center(FS_SM,240,235,120,"△ 刷新COS皮肤列表",C_MUTE);
    if(skin_catalog_state()==SKIN_CATALOG_ERROR)gfx_text_ellipsis(FS_SM,14,40,448,skin_catalog_error(),C_WARN);
    scrollbar_draw(top,view,count+1,view/row,(int)g_app.skin_scroll/row);hint_begin();hint_item("○","应用/安装");hint_item("□","预览");hint_item("SELECT","主题设置");hint_item("R","更新");hint_item("L","试听");hint_item("△","刷新");hint_item("×","设置");hint_end();
}

/* A feature module can be reused by Lua pages; its stock controls remain available
 * by returning false from skin.input. Coordinates define a viewport clip. */
#define SKIN_PAGE_MODULE(fn) static int skin_module_##fn(int x,int y,int w,int h){if(gfx_push_clip(x,y,x+w,y+h)<0)return 0;fn(g_app.time,.016f);gfx_pop_clip();return 1;}
static int skin_module_search_keyboard(int x,int y,int w,int h){if(gfx_push_clip(x,y,x+w,y+h)<0)return 0;screen_search(g_app.time,.016f);gfx_pop_clip();return 1;}
SKIN_PAGE_MODULE(screen_home) SKIN_PAGE_MODULE(screen_list) SKIN_PAGE_MODULE(screen_player) SKIN_PAGE_MODULE(screen_lyrics) SKIN_PAGE_MODULE(screen_search) SKIN_PAGE_MODULE(screen_download) SKIN_PAGE_MODULE(screen_cache) SKIN_PAGE_MODULE(screen_settings) SKIN_PAGE_MODULE(screen_help) SKIN_PAGE_MODULE(screen_login) SKIN_PAGE_MODULE(screen_grid) SKIN_PAGE_MODULE(screen_square) SKIN_PAGE_MODULE(screen_comments) SKIN_PAGE_MODULE(screen_mine) SKIN_PAGE_MODULE(screen_alt)
static int skin_module_status(int x,int y,int w,int h){if(gfx_push_clip(x,y,x+w,y+h)<0)return 0;screen_net_speed();gfx_pop_clip();return 1;}
static int skin_module_mini(int x,int y,int w,int h){if(gfx_push_clip(x,y,x+w,y+h)<0)return 0;player_bar_draw();gfx_pop_clip();return 1;}
static const SkinModule g_skin_modules[]={
 {"home",skin_module_screen_home,NULL},{"songs",skin_module_screen_list,NULL},{"player",skin_module_screen_player,NULL},{"lyrics",skin_module_screen_lyrics,NULL},{"search",skin_module_screen_search,NULL},{"search_keyboard",skin_module_search_keyboard,NULL},{"downloads",skin_module_screen_download,NULL},{"cache",skin_module_screen_cache,NULL},{"settings",skin_module_screen_settings,NULL},{"about",skin_module_screen_help,NULL},{"account",skin_module_screen_login,NULL},{"grid",skin_module_screen_grid,NULL},{"square",skin_module_screen_square,NULL},{"comments",skin_module_screen_comments,NULL},{"mine",skin_module_screen_mine,NULL},{"sources",skin_module_screen_alt,NULL},{"status",skin_module_status,NULL},{"mini_player",skin_module_mini,NULL}};

void ui_draw(void)
{
    static float last = 0;
    float t = g_app.time;
    float dt = t - last;

    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    last = t;

    g_app.enter = clamp01f(g_app.enter + dt * 6.0f);
    /* Function dialogs are immediate overlays; only page navigation turns. */
    int update_state=api_update_state();
    if(g_app.upd_dlg&&(update_state==4||update_state==7)){
        /* Do not redraw/queue home covers behind a modal bulk download. */
        gfx_clear(C_PANEL);upd_dialog_draw();
        skin_transition_draw(g_app.screen,t);
        screen_net_speed();toast_draw();return;
    }
    if(g_app.screen!=SC_SKINS)ui_free_skin_preview();
    views_refresh();
    skin_runtime_register(g_skin_modules,sizeof(g_skin_modules)/sizeof(g_skin_modules[0]));
    static const char*setting_labels[MELODY_SETTINGS_COUNT];static int setting_groups[MELODY_SETTINGS_COUNT];static char setting_values[MELODY_SETTINGS_COUNT][96];
    if(skin_runtime_active()&&g_app.screen==SC_SETTINGS){for(int i=0;i<MELODY_SETTINGS_COUNT;++i){setting_labels[i]=g_set_defs[i].label;setting_groups[i]=g_set_defs[i].group;settings_value(i,setting_values[i],sizeof(setting_values[i]));}skin_runtime_set_settings(setting_labels,setting_groups,setting_values,MELODY_SETTINGS_COUNT);}
    skin_runtime_set_cache(g_cache_view,g_cache_view_n);skin_runtime_set_cache_first(g_cache_view_first);
    skin_runtime_set_model(g_app.screen,&g_list,g_app.screen==SC_HOME?api_home_cards():api_cards());skin_runtime_set_playback(g_app.has_now?&g_app.now:NULL,audio_state(),audio_pos_ms(),audio_dur_ms());
    int custom=!(is_metro_style()&&g_app.screen==SC_WIFI)&&g_app.screen!=SC_SKINS&&g_app.screen!=SC_MV&&skin_runtime_draw(g_app.screen,t,dt);
    if(!custom)switch (g_app.screen) {
    case SC_BOOT:     screen_boot(t); break;
    case SC_WIFI:     screen_wifi(t, dt); break;
    case SC_HOME:     screen_home(t, dt); break;
    case SC_LIST:     screen_list(t, dt); break;
    case SC_PLAYER:   screen_player(t, dt); break;
    case SC_LYRICS:   screen_lyrics(t, dt); break;
    case SC_SEARCH:   screen_search(t, dt); break;
    case SC_DOWNLOAD: screen_download(t, dt); break;
    case SC_CACHE:    screen_cache(t, dt); break;
    case SC_SETTINGS: screen_settings(t, dt); break;
    case SC_HELP:     screen_help(t, dt); break;
    case SC_LOGIN:    screen_login(t, dt); break;
    case SC_GRID:     screen_grid(t, dt); break;
    case SC_SQUARE:   screen_square(t, dt); break;
    case SC_COMMENTS: screen_comments(t, dt); break;
    case SC_MINE:     screen_mine(t, dt); break;
    case SC_ALT:      screen_alt(t, dt); break;
    case SC_SKINS:screen_skins(t,dt);break;
    default: gfx_clear(C_BLACK); break;
    }

    skin_transition_draw(g_app.screen,t);
    if(!custom||!skin_runtime_style()->hide_mini)player_bar_draw();

    /* 进场淡入 */
    if(g_app.enter<1.0f&&(!custom||!skin_runtime_style()->hide_transition)){
        int a = (int)(210 * (1.0f - g_app.enter));

        gfx_fill_rect_a(0, 0, SCR_W, SCR_H, g_dark ? C_BLACK : C_WHITE, a);
    }

    if(is_metro_style()){
        if((g_app.busy&&g_app.screen!=SC_BOOT)||skin_transition_waiting()||(g_app.screen==SC_WIFI&&g_app.connecting==1))metro_loading_line(t);
    }else if(g_app.busy&&g_app.screen!=SC_BOOT&&g_app.screen!=SC_SEARCH)spinner_draw(g_app.busy_x,14,g_app.spinner);

    if(!custom||!skin_runtime_style()->hide_status)screen_net_speed();
    {
        const char *mode=!power_runtime_eco()?"常规模式":(power_runtime_ultra()?"极致省电":(power_runtime_locked()?"省电·皮肤":"省电·手动"));
        gfx_fill_rect(202,2,82,16,C_PANEL2);gfx_text(FS_SM,205,4,mode,C_ACC);
    }
    export_progress_draw();
    skin_options_dialog_draw();
    toast_draw();

    gfx_reset_clip();
    if (g_app.screen == SC_PLAYER && g_app.menu_open) player_menu_draw();
    if (g_app.confirm_clear) confirm_draw();
    if (g_app.upd_dlg) upd_dialog_draw();
    overclock_dialog_draw();
}






/* Pending MV uses ordinary UI buffers until the native presenter takes over.
 * The page must answer three questions: what is being fetched, how fast, and
 * why it has not started yet. */
void ui_mv_connecting(const char *title, long long available, long long total)
{
    unsigned short background=gfx_rgb(12,13,16),white=gfx_rgb(235,237,242);
    unsigned short muted=gfx_rgb(157,162,175),track=gfx_rgb(43,46,55);
    unsigned short hint=muted;
    char detail[96],stage[64],speed[80];
    long long rate=mv_stream_rate_bps(),need_rate=mv_stream_required_bps();
    int sustainable=mv_stream_sustainable();
    if(available<0)available=0;
    gfx_clear(background);
    gfx_text_center(FS_SM,16,SCR_W-32,30,"MELODY MV",muted);
    if(total<=0)snprintf(stage,sizeof(stage),"正在获取视频地址");
    else if(available<=0)snprintf(stage,sizeof(stage),"正在连接视频服务器");
    else if(!sustainable)snprintf(stage,sizeof(stage),"网速不足：先完整下载");
    else snprintf(stage,sizeof(stage),"正在缓冲，即将播放");
    gfx_text_center(FS_MD,20,SCR_W-40,88,stage,white);
    gfx_text_center(FS_SM,20,SCR_W-40,118,title?title:"",muted);
    gfx_fill_rect(48,155,SCR_W-96,4,track);
    /* This bar represents startup buffer, not full-file download completion. */
    long long need=mv_stream_active()?mv_stream_prefetch():total;
    if(total>0&&total<need)need=total;
    int filled=mv_buffer_percent(available,need)*(SCR_W-96)/100;
    gfx_fill_rect(48,155,filled,4,BRAND_RED());
    if(total>0){
        int startup=mv_buffer_percent(available,need),download=mv_buffer_percent(available,total);
        char sizes[48];fmt_done_total(sizes,sizeof(sizes),available,total);
        snprintf(detail,sizeof(detail),"%s · 起播 %d%% · 下载 %d%%",sizes,startup,download);
    } else snprintf(detail,sizeof(detail),"正在获取视频地址");
    gfx_text_center(FS_SM,20,SCR_W-40,171,detail,muted);
    if(total>0&&mv_stream_active()){
        long long eta=mv_stream_eta_ms();
        if(need_rate>0)snprintf(speed,sizeof(speed),"%lldKB/s · 视频约需 %lldKB/s",rate/1024,need_rate/1024);
        else snprintf(speed,sizeof(speed),"%lldKB/s",rate/1024);
        if(eta>0){int n=(int)strlen(speed);snprintf(speed+n,sizeof(speed)-n," · 剩余 %d:%02d",(int)(eta/60000),(int)(eta/1000%60));}
        gfx_text_center(FS_SM,20,SCR_W-40,193,speed,hint);
    } else if(total>0){
        gfx_text_center(FS_SM,20,SCR_W-40,193,"下载在后台继续，完成后自动开始播放",hint);
    }
    if(total>0&&!sustainable&&need_rate>0)
        gfx_text_center(FS_SM,10,SCR_W-30,232,"边下边播会卡：本次改为下完再播",gfx_rgb(245,169,87));
    int pulse=(int)(g_app.time*6.0f)%4;gfx_fill_rect(221+pulse*10,215,6,3,white);
    gfx_text_center(FS_SM,20,SCR_W-40,250,"X 取消并返回",white);
    screen_net_speed();
}

void ui_document(const char *title,const char *text,int scroll,const char *footer){
    if(is_metro_style()){
        gfx_clear(metro_background());gfx_fill_rect(0,0,SCR_W,3,C_ACC);
        gfx_text_ellipsis(FS_MD,20,12,SCR_W-40,title?title:"",C_TXT);
    }else{panel_bg();nav_page(title,"",0);}
    int row=0;const unsigned char*p=(const unsigned char*)(text?text:"");
    gfx_set_clip(20,35,SCR_W-40,205);
    while(*p){char line[160];int len=0,width=0;
        while(*p&&*p!='\n'){
            int bytes=*p<128?1:(*p&0xe0)==0xc0?2:(*p&0xf0)==0xe0?3:4;
            int pixels=*p<128?6:12;if(width+pixels>SCR_W-48||len+bytes>=(int)sizeof(line))break;
            int valid=1;for(int i=1;i<bytes;++i)if(!p[i]||(p[i]&0xc0)!=0x80){valid=0;break;}
            if(!valid)bytes=1;
            for(int i=0;i<bytes;++i)line[len++]=(char)*p++;
            width+=pixels;
        }
        line[len]=0;int y=38+(row-scroll)*18;if(y>=20&&y<=205)gfx_text(FS_SM,24,y,line,C_TXT);
        ++row;if(*p=='\n')++p;
    }
    gfx_reset_clip();gfx_fill_rect(0,216,SCR_W,56,C_BG);
    gfx_text_center(FS_SM,10,SCR_W-20,224,footer?footer:"",C_ACC);
    gfx_text_center(FS_SM,10,SCR_W-20,247,"上下 / 摇杆滚动",C_MUTE);screen_net_speed();
}
