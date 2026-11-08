/*
 * MELODY PSP 鈥?杞欢娓叉煋鍣ㄥ疄鐜? */

#include "gfx.h"
#include "gfx_gu.h"
#include "log.h"
#include "sysinfo.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspdmac.h>
#include <psputils.h>

#include <malloc.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_STRIDE 512
#define FONT_MAGIC 0x46505350u /* "PSPF" 灏忕 */

extern const unsigned char font12_blob[];
extern const unsigned char font12_blob_end[];
extern const unsigned char font16_blob[];
extern const unsigned char font16_blob_end[];
extern const unsigned char font22_blob[];
extern const unsigned char font22_blob_end[];
extern const unsigned char font32_blob[];
extern const unsigned char font32_blob_end[];

typedef struct {
    const unsigned char *bits;
    unsigned char w;
    unsigned char h;
} Glyph;

typedef struct {
    const unsigned char *blob;
    const unsigned char *blob_end;
    int count;
    int rec;
    const unsigned char *recs;
    int ready;
    int line_h;
} Font;

static unsigned short *g_fb[2];
static unsigned char *g_ram[2];        /* 缁樺埗鐩爣锛氭櫘閫氬唴瀛橈紙璧扮紦瀛橈紝蹇級 */
static void *g_vram[2];                /* 姣忓抚 memcpy 鐨勭洰鏍囷細VRAM 闈炵紦瀛樺埆鍚?*/
static void *g_fb_display[2];          /* 浜ょ粰 sceDisplaySetFrameBuf 鐨勫湴鍧€ */
static int g_back;
static int g_last = -1;
static int g_ready;
static int g_skip_copy;                /* MV writes VRAM directly */
static int g_immediate;                /* MV uses IMMEDIATE scanout */
static int g_direct_video;             /* single VRAM front buffer, no per-frame switch */

static int g_renderer,g_renderer_requested,g_ge_frame,g_renderer_suspended;
static unsigned g_render_started,g_render_frames,g_render_time_total,g_render_draws,g_render_uploads,g_render_syncs;
static char g_renderer_message[128];
static int gu_ready(void);
static void renderer_apply(void);
void gfx_cpu_barrier(void){if(g_renderer==GFX_RENDERER_GE&&g_ge_frame)gfx_gu_flush();}
void gfx_suspend_renderer(void){if(g_renderer==GFX_RENDERER_GE)mlog("renderer: suspend GE, retire commands and free 768KiB before MV");g_renderer_suspended=1;gfx_cpu_barrier();if(g_renderer==GFX_RENDERER_GE)for(int i=0;i<2;i++)memcpy(g_ram[i],g_vram[i],FB_STRIDE*SCR_H*2);gfx_gu_shutdown();g_ge_frame=0;g_renderer=GFX_RENDERER_CPU;for(int i=0;i<2;i++)g_fb[i]=(unsigned short*)g_ram[i];}
void gfx_resume_renderer(void){g_renderer_suspended=0;}
int gfx_renderer(void){return g_renderer_requested;}
const char*gfx_renderer_error(void){return g_renderer_message;}
int gfx_set_renderer(int renderer){if(renderer!=GFX_RENDERER_CPU&&renderer!=GFX_RENDERER_GE)return -1;if(renderer==GFX_RENDERER_GE&&g_ready&&!g_direct_video&&!g_renderer_suspended&&!gfx_gu_init()){snprintf(g_renderer_message,sizeof(g_renderer_message),"GE初始化失败，保留CPU");return -1;}g_renderer_requested=renderer;g_renderer_message[0]=0;return 0;}
static void renderer_apply(void){
 if(g_renderer==g_renderer_requested||g_direct_video||g_renderer_suspended)return;
 gfx_cpu_barrier();g_ge_frame=0;
 if(g_renderer_requested==GFX_RENDERER_GE){
  if(!gfx_gu_init()){snprintf(g_renderer_message,sizeof(g_renderer_message),"GE初始化失败，已使用CPU");g_renderer_requested=GFX_RENDERER_CPU;return;}
  for(int i=0;i<2;i++){memcpy(g_vram[i],g_ram[i],FB_STRIDE*SCR_H*2);g_fb[i]=(unsigned short*)g_vram[i];}
  g_renderer=GFX_RENDERER_GE;mlog("renderer switched to GE/GU");
 }else{for(int i=0;i<2;i++)memcpy(g_ram[i],g_vram[i],FB_STRIDE*SCR_H*2);gfx_gu_shutdown();for(int i=0;i<2;i++)g_fb[i]=(unsigned short*)g_ram[i];g_renderer=GFX_RENDERER_CPU;mlog("renderer switched to CPU");}
}
static int gu_ready(void){
 if(g_direct_video)return 0;
 renderer_apply();if(g_renderer!=GFX_RENDERER_GE)return 0;
 if(!g_ge_frame){g_render_started=sceKernelGetSystemTimeLow();if(!gfx_gu_begin((void*)(uintptr_t)(g_back?0x44000:0))){g_renderer_requested=GFX_RENDERER_CPU;renderer_apply();return 0;}g_ge_frame=1;}else gfx_gu_resume();
 return 1;
}

static Font g_fonts[FS_COUNT];
/* 主题自带字体：声明过的字号优先用它，缺字再回退内置。 */
static Font g_theme_fonts[FS_COUNT];

/* 姣忎釜瀛楀彿涓€琛屽崰鐨勯珮搴︼紙涔熸槸瀛楀簱鏍煎瓙鐨勯珮搴︼級 */
static const int g_line_h[FS_COUNT] = { 12, 16, 22, 32 };

/* 澶у瓧鍙锋壘涓嶅埌姹夊瓧鏃堕€愮骇閫€鍥炲皬瀛楀簱锛?2 -> 22 -> 16锛?*/
static const int g_chain[FS_COUNT][4] = {
    { FS_SM, -1, -1, -1 },
    { FS_MD, -1, -1, -1 },
    { FS_LG, FS_MD, -1, -1 },
    { FS_XL, FS_LG, FS_MD, -1 }
};

static void reset_clip(void);

/* ------------------------------------------------------------------ */
/* 鍒濆鍖?                                                             */
/* ------------------------------------------------------------------ */

static int font_load(Font *f, const unsigned char *blob, const unsigned char *end,
                     int line_h)
{
    const unsigned char *p = blob;
    unsigned int magic, version, count, rec_size, offset;
    size_t size = (size_t)(end - blob);

    f->ready = 0;
    f->line_h = line_h;
    if (size < 24) return -1;

    memcpy(&magic, p + 0, 4);
    memcpy(&version, p + 4, 4);
    memcpy(&count, p + 8, 4);
    memcpy(&rec_size, p + 12, 4);
    memcpy(&offset, p + 16, 4);

    if (magic != FONT_MAGIC || version != 1 || rec_size < 12 || offset > size)
        return -1;

    f->blob = blob;
    f->blob_end = end;
    f->count = (int)count;
    f->rec = (int)rec_size;
    f->recs = p + offset;
    f->ready = 1;
    return 0;
}

static void fonts_load(void)
{
    font_load(&g_fonts[FS_SM], font12_blob, font12_blob_end, 12);
    font_load(&g_fonts[FS_MD], font16_blob, font16_blob_end, 16);
    font_load(&g_fonts[FS_LG], font22_blob, font22_blob_end, 22);
    font_load(&g_fonts[FS_XL], font32_blob, font32_blob_end, 32);
}

void gfx_text_init(void)
{
    fonts_load();
}

static void mv_buf_prepare(void);   /* 启动时就把 MV 的 565 RAM 帧分配好 */

void gfx_init(void)
{
    unsigned char *vram = (unsigned char *)sceGeEdramGetAddr();
    int i;

    /*
     * 鐪熸満涓娿€岀洿鎺ュ線 VRAM 鐢汇€嶆湁涓ゆ潯璺兘涓嶇ǔ锛?     *   - 0x04000000 璧?D-cache锛?6KB 缂撳瓨 vs 272KB 涓€甯э紝writeback 涓嶅彲闈?鈫?榛戠嚎闂?     *   - 0x44000000 闈炵紦瀛橈細姣忔鍐欓兘杩囨€荤嚎锛屼竴甯ц 8ms 浠ヤ笂锛屽鏄撹刀涓嶄笂 vblank 鈫?闂睆
     *
     * 鎵€浠ユ敼鎴愶細鍦ㄦ櫘閫氬唴瀛橀噷鐢伙紙缂撳瓨銆佸揩锛夛紝姣忓抚涓€娆?memcpy 鍒?VRAM 鐨勯潪缂撳瓨鍒悕銆?     */
    for (i = 0; i < 2; ++i) {
        g_ram[i] = (unsigned char *)memalign(64, FB_STRIDE * SCR_H * 2);
        if (g_ram[i]) memset(g_ram[i], 0, FB_STRIDE * SCR_H * 2);
    }
    /* 注意：278KB 的 MV 565 帧**不能**在这里分配 —— 2MB 堆要留给 api_init
     * 的 1.3MB 缓冲，先占 278KB 会让 api 线程起不来。改为第一次进 MV、
     * api_free_buffers() 之后按需分配（见 gfx_begin_direct_video）。 */

    if (vram) {
        g_vram[0] = vram + 0x40000000;              /* 闈炵紦瀛樺埆鍚?*/
        g_vram[1] = vram + 0x40000000 + 0x44000;
        g_fb_display[0] = vram;                     /* 浜ょ粰鏄剧ず鎺у埗鍣ㄧ殑鍦板潃 */
        g_fb_display[1] = vram + 0x44000;
    } else {
        g_vram[0] = g_ram[0];
        g_vram[1] = g_ram[1];
        g_fb_display[0] = g_ram[0];
        g_fb_display[1] = g_ram[1];
    }

    g_fb[0] = (unsigned short *)g_ram[0];
    g_fb[1] = (unsigned short *)g_ram[1];

    g_back = 0;
    g_ready = (g_ram[0] && g_ram[1]) ? 1 : 0;

    if (g_ready) {
        sceDisplaySetFrameBuf(g_fb_display[1], FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565,
                              PSP_DISPLAY_SETBUF_NEXTFRAME);
    }

    fonts_load();
    reset_clip();
}

unsigned short *gfx_backbuffer(void)
{
    gfx_cpu_barrier();return g_fb[g_back];
}

void gfx_flip(void)
{
    if (!g_ready || g_direct_video) return;

    if(g_renderer==GFX_RENDERER_GE&&g_ge_frame){GfxGuStats stats;gfx_gu_end();gfx_gu_get_stats(&stats);g_render_frames++;g_render_time_total+=sceKernelGetSystemTimeLow()-g_render_started;g_render_draws+=stats.draws;g_render_uploads+=stats.uploads;g_render_syncs+=stats.flushes;if(g_render_frames%120==0){mlog("GE frames=%u avg_render_us=%u avg_draws=%u avg_uploads=%u avg_syncs=%u",g_render_frames,g_render_time_total/g_render_frames,g_render_draws/g_render_frames,g_render_uploads/g_render_frames,g_render_syncs/g_render_frames);}g_ge_frame=0;}
    g_last = g_back;

    /* 姝ｅ父 UI 鍦?RAM 缁樺埗锛屽鍒跺埌 VRAM锛汳V 鐩存帴鍐?VRAM锛岃烦杩囪繖娆?278KB memcpy銆?*/
    if (!g_skip_copy&&g_renderer!=GFX_RENDERER_GE) {
        if (g_immediate) {
            sceKernelDcacheWritebackRange(g_ram[g_back], FB_STRIDE * SCR_H * 2);
            sceDmacMemcpy(g_vram[g_back], g_ram[g_back], FB_STRIDE * SCR_H * 2);
        } else {
            memcpy(g_vram[g_back], g_ram[g_back], FB_STRIDE * SCR_H * 2);
        }
    }

    /* GE writes VRAM directly: after retiring its list, switch at this vblank.
     * NEXTFRAME here can defer adoption until the following vblank, while
     * g_back already reuses the old scanout for the next GE frame. CPU keeps
     * its existing RAM-render/copy scheduling. IMMEDIATE means next hsync. */
    sceDisplayWaitVblankStart();
    int display_result = sceDisplaySetFrameBuf(
        g_fb_display[g_back], FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565,
        (g_immediate || g_renderer == GFX_RENDERER_GE)
            ? PSP_DISPLAY_SETBUF_IMMEDIATE : PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (g_renderer == GFX_RENDERER_GE &&
        (g_render_frames == 1 || g_render_frames % 120 == 0)) {
        void *shown = NULL;
        int stride = 0, format = -1;
        int get_result = sceDisplayGetFrameBuf(
            &shown, &stride, &format, PSP_DISPLAY_SETBUF_IMMEDIATE);
        int mismatch = display_result < 0 || get_result < 0 ||
            shown != g_fb_display[g_back] || stride != FB_STRIDE ||
            format != PSP_DISPLAY_PIXEL_FORMAT_565;
        mlog("GE display%s frame=%u set=%08X get=%08X expected=%p shown=%p stride=%d fmt=%d",
             mismatch ? " mismatch" : "", g_render_frames,
             (unsigned)display_result, (unsigned)get_result,
             g_fb_display[g_back], shown, stride, format);
    }
    g_back ^= 1;
}

void gfx_set_immediate(int immediate)
{
    g_immediate = immediate ? 1 : 0;
}

/* MV 单缓冲：直接盖在 UI 的第 0 块 565 缓冲上（VRAM 偏移 0）。
 * 真机上 sceDisplaySetFrameBuf 切 8888 会返回 0x80000107（非法参数），
 * 565 正常 —— 所以 MV 也走 565：先把解码器的 RGBA 转成 565 放 RAM，
 * 再 DMA 到显存（半屏带宽，而且和 UI 用同一条 proven 路径）。 */
#define MV_FB_OFF 0

static unsigned short *g_mv_ram;    /* RAM 里的 565 帧（512x272） */
static void *g_mv_vram;             /* 非缓存显存别名，DMA 目标 */

void gfx_mv_set_osd(int on) { (void)on; }   /* 565 单缓冲下不做 OSD，保留接口 */
int gfx_mv_osd(void) { return 0; }

unsigned short *gfx_mv_ram(void) { return g_mv_ram; }
void *gfx_mv_vram(void) { return g_mv_vram; }

/* 启动时（堆几乎空）一次性分配 278KB 的 565 RAM 帧，之后永不释放。
 * 不能在每次进 MV 时分配：第二次进 MV 时 2MB 堆可能挤不出这么多。 */
static void mv_buf_prepare(void)
{
    if (g_mv_ram) return;
    g_mv_ram = (unsigned short *)memalign(64, FB_STRIDE * SCR_H * 2);
    if (g_mv_ram) memset(g_mv_ram, 0, FB_STRIDE * SCR_H * 2);
}

void gfx_begin_direct_video(void)
{
    unsigned char *vram = (unsigned char *)sceGeEdramGetAddr();
    unsigned short *p;
    void *got = NULL;
    int gw = 0, gf = 0;
    int r;

    if (!g_ready || !vram) return;
    gfx_suspend_renderer();
    mv_buf_prepare();
    if (!g_mv_ram) {
        mlog("mv: no ram buffer for video");
        return;
    }
    g_mv_vram = vram + 0x40000000 + MV_FB_OFF;
    g_direct_video = 1;
    g_skip_copy = 1;
    g_immediate = 1;
    g_back = 0;

    /* 先清黑一次，免得第一帧之前是上一轮 UI 的残留 */
    for (int i = 0; i < FB_STRIDE * SCR_H; ++i) g_mv_ram[i] = 0;
    sceKernelDcacheWritebackRange(g_mv_ram, FB_STRIDE * SCR_H * 2);
    p = (unsigned short *)g_mv_vram;
    for (int i = 0; i < FB_STRIDE * SCR_H; ++i) p[i] = 0;

    sceDisplayWaitVblankStart();
    r = sceDisplaySetFrameBuf(vram + MV_FB_OFF, FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565,
                              PSP_DISPLAY_SETBUF_IMMEDIATE);
    sceDisplayGetFrameBuf(&got, &gw, &gf, PSP_DISPLAY_SETBUF_IMMEDIATE);
    mlog("mv: disp off=%X set=%08X get=%08X w=%d fmt=%d vc=%u free=%dK max=%dK",
         MV_FB_OFF, r, (unsigned)got, gw, gf, sceDisplayGetVcount(),
         sysi_free_kb(), sysi_maxblock_kb());
}

void gfx_end_direct_video(void)
{
    if (g_direct_video) mlog("mv: end direct video");
    g_direct_video = 0;
    g_renderer_suspended=0;
    g_skip_copy = 0;
    g_immediate = 0;
    g_back = 0;
    g_mv_vram = NULL;

    /* 278KB 的 MV 565 帧只在 MV 期间用，退出时还给 2MB 堆，
     * 让 api_init() 重新分配 1.3MB 缓冲时不至于太紧。 */
    if (g_mv_ram) { free(g_mv_ram); g_mv_ram = NULL; }

    if (g_ready) {
        sceDisplayWaitVblankStart();
        sceDisplaySetFrameBuf(g_fb_display[0], FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565,
                              PSP_DISPLAY_SETBUF_IMMEDIATE);
    }
}

/* gfx_mv_fb / gfx_mv_ram / gfx_mv_vram 已在上面定义 */

void gfx_set_skip_copy(int skip)
{
    g_skip_copy = skip ? 1 : 0;
}

unsigned short *gfx_vram_backbuffer(void)
{
    if (!g_ready) return NULL;
    gfx_cpu_barrier();return (unsigned short *)g_vram[g_direct_video ? 0 : g_back];
}

/* ------------------------------------------------------------------ */
/* 棰滆壊                                                                */
/* ------------------------------------------------------------------ */

unsigned short gfx_rgb(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (unsigned short)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}

static inline unsigned short blend(unsigned short dst, unsigned short src, int a)
{
    int dr = dst & 0x1F, dg = (dst >> 5) & 0x3F, db = (dst >> 11) & 0x1F;
    int sr = src & 0x1F, sg = (src >> 5) & 0x3F, sb = (src >> 11) & 0x1F;

    dr += ((sr - dr) * a) >> 8;
    dg += ((sg - dg) * a) >> 8;
    db += ((sb - db) * a) >> 8;
    return (unsigned short)(dr | (dg << 5) | (db << 11));
}

unsigned short gfx_lerp565(unsigned short a, unsigned short b, int t)
{
    if (t <= 0) return a;
    if (t >= 256) return b;
    return blend(a, b, t);
}

/* ------------------------------------------------------------------ */
/* 瑁佸壀                                                                */
/* ------------------------------------------------------------------ */

static unsigned short *g_target; /* NULL = 褰撳墠鍚庣紦鍐?*/
static int g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;
typedef struct{int x0,y0,x1,y1,sx0,sy0,sx1,sy1;}ClipScope;
static ClipScope g_clip_scopes[4];static int g_clip_scope_count;

static unsigned short *fb(void)
{
    return g_target ? g_target : g_fb[g_back];
}

static void set_clip(int x0, int y0, int x1, int y1)
{
    if(g_clip_scope_count){const ClipScope*c=&g_clip_scopes[g_clip_scope_count-1];if(x0<c->x0)x0=c->x0;if(y0<c->y0)y0=c->y0;if(x1>c->x1)x1=c->x1;if(y1>c->y1)y1=c->y1;if(x1<=x0||y1<=y0)x0=y0=x1=y1=0;}
    g_clip_x0 = x0;
    g_clip_y0 = y0;
    g_clip_x1 = x1;
    g_clip_y1 = y1;
    if(g_renderer==GFX_RENDERER_GE&&g_ge_frame)gfx_gu_clip(x0,y0,x1,y1);
}

static void reset_clip(void)
{
    set_clip(0, 0, SCR_W, SCR_H);
}

void gfx_set_clip(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 0; }
    set_clip(x0, y0, x1, y1);
}

void gfx_reset_clip(void)
{
    reset_clip();
}

int gfx_push_clip(int x0,int y0,int x1,int y1){if(g_clip_scope_count>=4)return -1;ClipScope*c=&g_clip_scopes[g_clip_scope_count];c->sx0=g_clip_x0;c->sy0=g_clip_y0;c->sx1=g_clip_x1;c->sy1=g_clip_y1;if(x0<g_clip_x0)x0=g_clip_x0;if(y0<g_clip_y0)y0=g_clip_y0;if(x1>g_clip_x1)x1=g_clip_x1;if(y1>g_clip_y1)y1=g_clip_y1;if(x1<=x0||y1<=y0)x0=y0=x1=y1=0;c->x0=x0;c->y0=y0;c->x1=x1;c->y1=y1;++g_clip_scope_count;set_clip(x0,y0,x1,y1);return 0;}
void gfx_pop_clip(void){if(!g_clip_scope_count)return;ClipScope saved=g_clip_scopes[--g_clip_scope_count];set_clip(saved.sx0,saved.sy0,saved.sx1,saved.sy1);}

void gfx_get_clip(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = g_clip_x0;
    if (y0) *y0 = g_clip_y0;
    if (x1) *x1 = g_clip_x1;
    if (y1) *y1 = g_clip_y1;
}

static inline int clip_rect(int *x, int *y, int *w, int *h)
{
    if (*x < g_clip_x0) { *w += *x - g_clip_x0; *x = g_clip_x0; }
    if (*y < g_clip_y0) { *h += *y - g_clip_y0; *y = g_clip_y0; }
    if (*x + *w > g_clip_x1) *w = g_clip_x1 - *x;
    if (*y + *h > g_clip_y1) *h = g_clip_y1 - *y;
    return (*w > 0 && *h > 0);
}

/* ------------------------------------------------------------------ */
/* 鍩虹鍥惧厓                                                            */
/* ------------------------------------------------------------------ */

/* 涓€琛屽悓鑹插～鍏咃細瀵归綈鍚?32 浣嶅啓锛孭SP 涓婃瘮閫愬儚绱犲揩涓€鍊嶅 */
static inline void fill_row(unsigned short *p, int w, unsigned short c)
{
    unsigned int c32 = ((unsigned int)c << 16) | c;
    int i;

    if (w <= 0) return;

    /* PSP 鐨?32 浣?store 蹇呴』 4 瀛楄妭瀵归綈锛氳棣栧儚绱犳槸濂囨暟鏃跺厛琛ヤ竴涓?16 浣嶅啓锛?     * 鍐嶆妸闀垮害鍑戞垚鍋舵暟銆備互鍓嶅彧鍒ゆ柇浜?w 鐨勫鍋讹紝娌″垽鏂湴鍧€锛屽渾/鍦嗚鐭╁舰鐨?     * 濂囨暟 x 浼氳蛋闈炲榻?32 浣嶅啓 鈫?鐪熸満鐩存帴鍦板潃閿欒閲嶅惎锛圥PSSPP 涓嶅鐜帮級銆?*/
    if ((unsigned int)p & 2) {
        *p++ = c;
        w--;
    }
    if (w <= 0) return;
    if (w & 1) {
        p[w - 1] = c;
        w--;
    }
    for (i = 0; i < w; i += 2) {
        unsigned int *q = (unsigned int *)(p + i);
        *q = c32;
    }
}

void gfx_clear(unsigned short color)
{
    if(gu_ready()){gfx_gu_clip(0,0,SCR_W,SCR_H);gfx_gu_rect(0,0,SCR_W,SCR_H,color,255);gfx_gu_clip(g_clip_x0,g_clip_y0,g_clip_x1,g_clip_y1);return;}
    unsigned short *p = fb();
    int y;

    if (!p) return;
    for (y = 0; y < SCR_H; ++y) fill_row(p + y * FB_STRIDE, SCR_W, color);
}

void gfx_pixel(int x, int y, unsigned short color)
{
    if(gu_ready()){gfx_gu_rect(x,y,1,1,color,255);return;}
    if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) return;
    fb()[y * FB_STRIDE + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned short color)
{
    if(gu_ready()){gfx_gu_rect(x,y,w,h,color,255);return;}
    unsigned short *p = fb();
    int yy;

    if (!p || !clip_rect(&x, &y, &w, &h)) return;

    for (yy = 0; yy < h; ++yy)
        fill_row(p + (y + yy) * FB_STRIDE + x, w, color);
}

void gfx_fill_rect_a(int x, int y, int w, int h, unsigned short color, int alpha)
{
    if(gu_ready()){gfx_gu_rect(x,y,w,h,color,alpha);return;}
    unsigned short *p = fb();
    int yy;

    if (alpha <= 0) return;
    if (alpha >= 255) { gfx_fill_rect(x, y, w, h, color); return; }
    if (!p || !clip_rect(&x, &y, &w, &h)) return;

    for (yy = 0; yy < h; ++yy) {
        unsigned short *row = p + (y + yy) * FB_STRIDE + x;
        int xx;
        for (xx = 0; xx < w; ++xx) row[xx] = blend(row[xx], color, alpha);
    }
}

void gfx_hline(int x, int y, int w, unsigned short color)
{
    gfx_fill_rect(x, y, w, 1, color);
}

void gfx_vline(int x, int y, int h, unsigned short color)
{
    gfx_fill_rect(x, y, 1, h, color);
}

static unsigned int isqrt32(unsigned int v)
{
    unsigned int r = 0, bit = 1u << 30;

    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* 涓€琛屽渾瑙掔煩褰㈢殑宸﹁竟鍐呯缉閲?*/
static inline int round_inset(int r, int row, int h)
{
    int dy;

    if (row < r) dy = r - row;
    else if (row >= h - r) dy = row - (h - r - 1);
    else return 0;

    if (dy >= r) return r;
    {
        int rr = r * r - dy * dy;
        if (rr < 0) return r;
        return r - (int)isqrt32((unsigned int)rr);
    }
}

void gfx_fill_round(int x, int y, int w, int h, int r, unsigned short color)
{
    gfx_fill_round_a(x, y, w, h, r, color, 255);
}

void gfx_fill_round_a(int x, int y, int w, int h, int r, unsigned short color, int alpha)
{
    int ge=gu_ready();
    unsigned short *p = fb();
    int row;

    if (!p || w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;

    for (row = 0; row < h; ++row) {
        int dy = y + row;
        int inset = r ? round_inset(r, row, h) : 0;
        int rx = x + inset;
        int rw = w - inset * 2;

        if (dy < g_clip_y0 || dy >= g_clip_y1) continue;
        if (rx < g_clip_x0) { rw += rx - g_clip_x0; rx = g_clip_x0; }
        if (rx + rw > g_clip_x1) rw = g_clip_x1 - rx;
        if (rw <= 0) continue;
        if(ge){gfx_gu_rect(rx,dy,rw,1,color,alpha);continue;}

        {
            unsigned short *rowp = p + dy * FB_STRIDE + rx;
            if (alpha >= 255) {
                fill_row(rowp, rw, color);
            } else if (alpha > 0) {
                int i;
                for (i = 0; i < rw; ++i) rowp[i] = blend(rowp[i], color, alpha);
            }
        }
    }
}

void gfx_round_border(int x, int y, int w, int h, int r, unsigned short color)
{
    gfx_round_border_a(x, y, w, h, r, color, 255);
}

void gfx_round_border_a(int x, int y, int w, int h, int r, unsigned short color, int alpha)
{
    int row;

    if (w <= 0 || h <= 0) return;

    for (row = 0; row < h; ++row) {
        int inset = r ? round_inset(r, row, h) : 0;
        int first = x + inset;
        int last = x + w - 1 - inset;
        int dy = y + row;

        if (dy < g_clip_y0 || dy >= g_clip_y1) continue;
        if (row < 1 || row > h - 2) {
            gfx_fill_rect_a(first, dy, last - first + 1, 1, color, alpha);
        } else {
            gfx_fill_rect_a(first, dy, 1, 1, color, alpha);
            gfx_fill_rect_a(last, dy, 1, 1, color, alpha);
        }
    }
}

void gfx_vgrad(int x, int y, int w, int h, unsigned short top, unsigned short bottom)
{
    int ge=gu_ready();
    unsigned short *p = fb();
    int yy;

    if (!p || h <= 0) return;
    if (!clip_rect(&x, &y, &w, &h)) return;

    for (yy = 0; yy < h; ++yy) {
        int t = (h > 1) ? (yy * 256 / (h - 1)) : 0;
        unsigned short c = gfx_lerp565(top, bottom, t);
        if(ge)gfx_gu_rect(x,y+yy,w,1,c,255);else fill_row(p+(y+yy)*FB_STRIDE+x,w,c);
    }
}

void gfx_hgrad(int x, int y, int w, int h, unsigned short left, unsigned short right)
{
    int ge=gu_ready();
    unsigned short *p = fb();
    int xx;

    if (!p || w <= 0) return;
    if (!clip_rect(&x, &y, &w, &h)) return;

    for (xx = 0; xx < w; ++xx) {
        int t = (w > 1) ? (xx * 256 / (w - 1)) : 0;
        unsigned short c = gfx_lerp565(left, right, t);
        int yy;
        if(ge)gfx_gu_rect(x+xx,y,1,h,c,255);else for(yy=0;yy<h;++yy)p[(y+yy)*FB_STRIDE+x+xx]=c;
    }
}

void gfx_fill_round_grad(int x, int y, int w, int h, int r,
                         unsigned short top, unsigned short bottom)
{
    int ge=gu_ready();
    unsigned short *p = fb();
    int row;

    if (!p || w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;

    for (row = 0; row < h; ++row) {
        int dy = y + row;
        int t = (h > 1) ? (row * 256 / (h - 1)) : 0;
        unsigned short c = gfx_lerp565(top, bottom, t);
        int inset = r ? round_inset(r, row, h) : 0;
        int rx = x + inset;
        int rw = w - inset * 2;

        if (dy < g_clip_y0 || dy >= g_clip_y1) continue;
        if (rx < g_clip_x0) { rw += rx - g_clip_x0; rx = g_clip_x0; }
        if (rx + rw > g_clip_x1) rw = g_clip_x1 - rx;
        if (rw <= 0) continue;

        if(ge)gfx_gu_rect(rx,dy,rw,1,c,255);else fill_row(p+dy*FB_STRIDE+rx,rw,c);
    }
}

void gfx_circle(int cx, int cy, int r, unsigned short color)
{
    gfx_circle_a(cx, cy, r, color, 255);
}

void gfx_circle_a(int cx, int cy, int r, unsigned short color, int alpha)
{
    gfx_ring_a(cx, cy, r, 0, color, alpha);
}

void gfx_ring(int cx, int cy, int r_out, int r_in, unsigned short color)
{
    gfx_ring_a(cx, cy, r_out, r_in, color, 255);
}

void gfx_ring_a(int cx, int cy, int r_out, int r_in, unsigned short color, int alpha)
{
    unsigned short *p = fb();
    int dy;

    if (!p || r_out <= 0) return;

    for (dy = -r_out; dy <= r_out; ++dy) {
        int y = cy + dy;
        int dx, x0, x1;

        if (y < g_clip_y0 || y >= g_clip_y1) continue;

        dx = (int)isqrt32((unsigned int)(r_out * r_out - dy * dy));
        x0 = cx - dx;
        x1 = cx + dx;

        if (r_in > 0) {
            int in_dy = dy < 0 ? -dy : dy;
            if (in_dy < r_in) {
                int id = (int)isqrt32((unsigned int)(r_in * r_in - dy * dy));
                /* 宸﹀彸涓ゆ潯 */
                int lx0 = cx - dx, lx1 = cx - id;
                int rx0 = cx + id, rx1 = cx + dx;
                gfx_fill_rect_a(lx0, y, lx1 - lx0 + 1, 1, color, alpha);
                gfx_fill_rect_a(rx0, y, rx1 - rx0 + 1, 1, color, alpha);
                continue;
            }
        }

        gfx_fill_rect_a(x0, y, x1 - x0 + 1, 1, color, alpha);
    }
}

void gfx_arc(int cx, int cy, int r_out, int r_in, int a0, int a1,
             unsigned short color, int alpha)
{
    int steps;
    int prev_x = 0, prev_y = 0;

    if (a1 < a0) { int t = a0; a0 = a1; a1 = t; }
    if (a1 - a0 > 360) a1 = a0 + 360;

    steps = (a1 - a0) * r_out / 90;
    if (steps < 8) steps = 8;
    if (steps > 720) steps = 720;

    for (int i = 0; i <= steps; ++i) {
        double ang = (a0 + (a1 - a0) * (double)i / steps) * 3.14159265358979 / 180.0;
        int ox = cx + (int)(cos(ang) * r_out);
        int oy = cy + (int)(sin(ang) * r_out);

        if (i > 0) {
            /* 涓婁竴娈靛埌杩欎竴娈电殑寰勫悜杩炵嚎 */
            int dx = ox - prev_x, dy = oy - prev_y;
            int n = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (n < 1) n = 1;
            for (int s = 0; s <= n; ++s) {
                int x = prev_x + dx * s / n;
                int y = prev_y + dy * s / n;
                int tx = ox - (cx + (int)(cos(ang) * r_in));
                int ty = oy - (cy + (int)(sin(ang) * r_in));
                int tn = (tx < 0 ? -tx : tx) + (ty < 0 ? -ty : ty);
                if (tn < 1) tn = 1;
                for (int t = 0; t <= tn; ++t) {
                    int px = x - tx * t / tn;
                    int py = y - ty * t / tn;
                    if (px < 0 || py < 0 || px >= SCR_W || py >= SCR_H) continue;
                    if(gu_ready())gfx_gu_rect(px,py,1,1,color,alpha);
                    else if(alpha>=255)fb()[py*FB_STRIDE+px]=color;
                    else fb()[py*FB_STRIDE+px]=blend(fb()[py*FB_STRIDE+px],color,alpha);
                }
            }
        }
        prev_x = ox;
        prev_y = oy;
    }
}

void gfx_tri(int x0, int y0, int x1, int y1, int x2, int y2,
             unsigned short color, int alpha)
{
    int ymin = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int ymax = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);

    for (int y = ymin; y <= ymax; ++y) {
        float xs[3];
        int n = 0;

        int ys[3] = { y0, y1, y2 };
        int xs3[3] = { x0, x1, x2 };

        for (int i = 0; i < 3; ++i) {
            int j = (i + 1) % 3;
            if (ys[i] == ys[j]) continue;
            if ((y >= ys[i] && y <= ys[j]) || (y >= ys[j] && y <= ys[i])) {
                float t = (float)(y - ys[i]) / (float)(ys[j] - ys[i]);
                xs[n++] = xs3[i] + (xs3[j] - xs3[i]) * t;
            }
        }
        if (n == 0) continue;

        {
            float a = xs[0], b = xs[0];
            for (int i = 1; i < n; ++i) {
                if (xs[i] < a) a = xs[i];
                if (xs[i] > b) b = xs[i];
            }
            gfx_fill_rect_a((int)a, y, (int)(b - a) + 1, 1, color, alpha);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 浣嶅浘                                                                */
/* ------------------------------------------------------------------ */

void gfx_blit_rgba_quad(const unsigned char*src,int sw,int sh,const float xy[8],const float uv[8],int alpha){
 if(!src||!xy||!uv||sw<1||sh<1||sw>256||sh>256||alpha<=0)return;
 for(int i=0;i<8;++i)if(!isfinite(xy[i])||!isfinite(uv[i])||xy[i]<-1024||xy[i]>2048)return;
 if(alpha>255)alpha=255;
 if(gu_ready()&&gfx_gu_quad8888_alpha(src,sw,sh,xy,uv,alpha))return;
 gfx_cpu_barrier();unsigned short*target=fb();if(!target)return;
 /* CPU compatibility for the vertical mesh strips used by the wave renderer. */
 float left=xy[0],right=xy[2];if(right<=left||fabsf(xy[4]-left)>.01f||fabsf(xy[6]-right)>.01f)return;
 int x0=(int)ceilf(left),x1=(int)ceilf(right);if(x0<g_clip_x0)x0=g_clip_x0;if(x1>g_clip_x1)x1=g_clip_x1;
 for(int x=x0;x<x1;++x){float f=(x+.5f-left)/(right-left),top=xy[1]+(xy[3]-xy[1])*f,bottom=xy[5]+(xy[7]-xy[5])*f;if(bottom<=top)continue;
  int y0=(int)ceilf(top),y1=(int)ceilf(bottom);if(y0<g_clip_y0)y0=g_clip_y0;if(y1>g_clip_y1)y1=g_clip_y1;
  int sx=(int)(uv[0]+(uv[2]-uv[0])*f);if(sx<0)sx=0;if(sx>=sw)sx=sw-1;
  float vt=uv[1]+(uv[3]-uv[1])*f,vb=uv[5]+(uv[7]-uv[5])*f;int step=(int)((vb-vt)*65536.f/(bottom-top));int v=(int)((vt+(y0+.5f-top)*(vb-vt)/(bottom-top))*65536.f);
  for(int y=y0;y<y1;++y,v+=step){int sy=v>>16;if(sy<0)sy=0;if(sy>=sh)sy=sh-1;const unsigned char*p=src+((size_t)sy*sw+sx)*4;int a=p[3]*alpha/255;if(a){unsigned short*out=target+y*FB_STRIDE+x,c=gfx_rgb(p[0],p[1],p[2]);*out=a==255?c:blend(*out,c,a);}}
 }
}

void gfx_blit_rgba_rotated(int x,int y,int size,const unsigned char*src,int sw,int sh,float angle){
 if(!src||size<=0||size>224||sw<=0||sh<=0||sw>256||sh>256||!isfinite(angle))return;
 float radians=fmodf(angle,360.f)*.01745329252f,co=cosf(radians),si=sinf(radians),half=size*.5f,cx=x+half,cy=y+half;
 float xy[8],uv[8]={0,0,(float)sw,0,0,(float)sh,(float)sw,(float)sh};
 for(int i=0;i<4;++i){float dx=(i%2?half:-half),dy=(i>=2?half:-half);xy[i*2]=cx+dx*co-dy*si;xy[i*2+1]=cy+dx*si+dy*co;}
 if(gu_ready()&&gfx_gu_quad8888(src,sw,sh,xy,uv))return;
 gfx_cpu_barrier();unsigned short*target=fb();if(!target)return;
 int x0=x>g_clip_x0?x:g_clip_x0,y0=y>g_clip_y0?y:g_clip_y0,x1=x+size<g_clip_x1?x+size:g_clip_x1,y1=y+size<g_clip_y1?y+size:g_clip_y1;
 for(int py=y0;py<y1;++py)for(int px=x0;px<x1;++px){float dx=px+.5f-cx,dy=py+.5f-cy;int sx=(int)((dx*co+dy*si+half)*sw/size),sy=(int)((-dx*si+dy*co+half)*sh/size);if(sx<0||sy<0||sx>=sw||sy>=sh)continue;const unsigned char*p=src+((size_t)sy*sw+sx)*4;if(p[3]){unsigned short*out=target+py*FB_STRIDE+px,c=gfx_rgb(p[0],p[1],p[2]);*out=p[3]==255?c:blend(*out,c,p[3]);}}
}

void gfx_blit_rgba_scaled(int x,int y,int w,int h,const unsigned char*rgba,int sw,int sh,int alpha){
 if(gu_ready()&&gfx_gu_image(x,y,w,h,rgba,sw,sh,sw,GFX_GU_8888,alpha))return;gfx_cpu_barrier();
 if(!rgba||w<=0||h<=0||w>960||h>544||sw<=0||sh<=0||sw>512||sh>512||alpha<=0)return;if(alpha>255)alpha=255;unsigned short*target=fb();if(!target)return;
 int x0=x<g_clip_x0?g_clip_x0:x,y0=y<g_clip_y0?g_clip_y0:y,x1=x+w>g_clip_x1?g_clip_x1:x+w,y1=y+h>g_clip_y1?g_clip_y1:y+h;
 for(int py=y0;py<y1;++py){int sy=(py-y)*sh/h;for(int px=x0;px<x1;++px){int sx=(px-x)*sw/w;const unsigned char*p=rgba+((size_t)sy*sw+sx)*4;int a=p[3]*alpha/255;if(a){unsigned short c=gfx_rgb(p[0],p[1],p[2]);unsigned short*out=target+py*FB_STRIDE+px;*out=a==255?c:blend(*out,c,a);}}}
}
void gfx_blit565_cover(int x,int y,int w,int h,const unsigned short*src,int sw,int sh){
 if(src&&w>0&&h>0&&sw>0&&sh>0&&gu_ready()){
  int cw=sw,ch=sh;if((long long)sw*h>(long long)sh*w)cw=(int)((long long)sh*w/h);else ch=(int)((long long)sw*h/w);if(cw<1)cw=1;if(ch<1)ch=1;float ox=(sw-cw)*.5f,oy=(sh-ch)*.5f;
  float xy[8]={(float)x,(float)y,(float)(x+w),(float)y,(float)x,(float)(y+h),(float)(x+w),(float)(y+h)};float uv[8]={ox,oy,ox+cw,oy,ox,oy+ch,ox+cw,oy+ch};if(gfx_gu_quad565(src,sw,sh,sw,xy,uv,0xffff))return;
 }gfx_cpu_barrier();
 if(!src||w<=0||h<=0||w>960||h>544||sw<=0||sh<=0)return;unsigned short*target=fb();if(!target)return;
 int cropw=sw,croph=sh;if((long long)sw*h>(long long)sh*w)cropw=(int)((long long)sh*w/h);else croph=(int)((long long)sw*h/w);if(cropw<1)cropw=1;if(croph<1)croph=1;int ox=(sw-cropw)/2,oy=(sh-croph)/2;
 int x0=x<g_clip_x0?g_clip_x0:x,y0=y<g_clip_y0?g_clip_y0:y,x1=x+w>g_clip_x1?g_clip_x1:x+w,y1=y+h>g_clip_y1?g_clip_y1:y+h;
 for(int py=y0;py<y1;++py){int sy=oy+(py-y)*croph/h;for(int px=x0;px<x1;++px)target[py*FB_STRIDE+px]=src[sy*sw+ox+(px-x)*cropw/w];}
}
void gfx_blit565_scaled(int x,int y,int w,int h,const unsigned short*src,int sw,int sh){
 if(gu_ready()&&gfx_gu_image(x,y,w,h,src,sw,sh,sw,GFX_GU_5650,255))return;gfx_cpu_barrier();
 if(!src||w<=0||h<=0||w>960||h>544||sw<=0||sh<=0)return;unsigned short*target=fb();if(!target)return;
 int x0=x<g_clip_x0?g_clip_x0:x,y0=y<g_clip_y0?g_clip_y0:y,x1=x+w>g_clip_x1?g_clip_x1:x+w,y1=y+h>g_clip_y1?g_clip_y1:y+h;
 for(int py=y0;py<y1;++py){int sy=(py-y)*sh/h;for(int px=x0;px<x1;++px)target[py*FB_STRIDE+px]=src[sy*sw+(px-x)*sw/w];}
}

/* 鏁村潡閮藉湪灞忓箷鍐呮椂璧?memcpy锛堝皝闈€佽儗鏅渶甯哥敤锛?*/
static inline int rect_inside(int x, int y, int w, int h)
{
    return x >= 0 && y >= 0 && x + w <= SCR_W && y + h <= SCR_H;
}

void gfx_blit565(int x, int y, int w, int h, const unsigned short *src, int stride)
{
    if(gu_ready()&&gfx_gu_image(x,y,w,h,src,w,h,stride,GFX_GU_5650,255))return;gfx_cpu_barrier();
    unsigned short *p = fb();

    if (!p || !src) return;

    if (rect_inside(x, y, w, h) && x >= g_clip_x0 && y >= g_clip_y0 &&
        x + w <= g_clip_x1 && y + h <= g_clip_y1) {
        for (int yy = 0; yy < h; ++yy)
            memcpy(p + (y + yy) * FB_STRIDE + x, src + (size_t)yy * stride,
                   (size_t)w * 2);
        return;
    }

    for (int yy = 0; yy < h; ++yy) {
        int py = y + yy;
        if (py < g_clip_y0 || py >= g_clip_y1) continue;
        for (int xx = 0; xx < w; ++xx) {
            int px = x + xx;
            if (px < g_clip_x0 || px >= g_clip_x1) continue;
            p[py * FB_STRIDE + px] = src[yy * stride + xx];
        }
    }
}

void gfx_blit565_a(int x, int y, int w, int h, const unsigned short *src, int stride, int alpha)
{
    if(gu_ready()&&gfx_gu_image(x,y,w,h,src,w,h,stride,GFX_GU_5650,alpha))return;gfx_cpu_barrier();
    unsigned short *p = fb();

    if (!p || !src) return;
    if (alpha >= 255) { gfx_blit565(x, y, w, h, src, stride); return; }
    if (alpha <= 0) return;

    for (int yy = 0; yy < h; ++yy) {
        int py = y + yy;
        if (py < g_clip_y0 || py >= g_clip_y1) continue;
        for (int xx = 0; xx < w; ++xx) {
            int px = x + xx;
            if (px < g_clip_x0 || px >= g_clip_x1) continue;
            p[py * FB_STRIDE + px] = blend(p[py * FB_STRIDE + px], src[yy * stride + xx], alpha);
        }
    }
}

void gfx_blit565_scale(int x, int y, int dw, int dh,
                       const unsigned short *src, int sw, int sh)
{
    if(gu_ready()&&gfx_gu_image(x,y,dw,dh,src,sw,sh,sw,GFX_GU_5650,255))return;gfx_cpu_barrier();
    unsigned short *p = fb();

    if (!p || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    for (int yy = 0; yy < dh; ++yy) {
        int py = y + yy;
        const unsigned short *srow = src + (size_t)(yy * sh / dh) * sw;
        if (py < g_clip_y0 || py >= g_clip_y1) continue;
        for (int xx = 0; xx < dw; ++xx) {
            int px = x + xx;
            if (px < g_clip_x0 || px >= g_clip_x1) continue;
            p[py * FB_STRIDE + px] = srow[xx * sw / dw];
        }
    }
}

static inline int in_round_rect(int dx, int dy, int w, int h, int r)
{
    int cx, cy;

    if (dx < 0 || dy < 0 || dx >= w || dy >= h) return 0;
    if (r <= 0 || w <= 2 * r || h <= 2 * r) return 1;
    if (dx < r && dy < r) { cx = r - dx - 1; cy = r - dy - 1; }
    else if (dx >= w - r && dy < r) { cx = dx - (w - r); cy = r - dy - 1; }
    else if (dx < r && dy >= h - r) { cx = r - dx - 1; cy = dy - (h - r); }
    else if (dx >= w - r && dy >= h - r) { cx = dx - (w - r); cy = dy - (h - r); }
    else return 1;

    return cx * cx + cy * cy <= r * r;
}

/* 缂╂斁 + 鍦嗚閬僵锛氬洓瑙掔湡鐨勯€忔槑锛堜笉浼氭嬁闈㈡澘鑹插幓娑傦級 */
void gfx_blit565_round(int x, int y, int dw, int dh,
                       const unsigned short *src, int sw, int sh, int r, int alpha)
{
    if(src&&dw>0&&dh>0&&sw>0&&sh>0&&sw<=512&&sh<=512&&alpha>0&&gu_ready()){
        if(r<0)r=0;if(r>dw/2)r=dw/2;if(r>dh/2)r=dh/2;
        for(int row=0;row<dh;++row){int inset=round_inset(r,row,dh);int left=x+inset,right=x+dw-inset,top=y+row;
            if(left<g_clip_x0)left=g_clip_x0;if(right>g_clip_x1)right=g_clip_x1;if(top<g_clip_y0||top>=g_clip_y1||right<=left)continue;
            gfx_gu_clip(left,top,right,top+1);gfx_gu_image(x,y,dw,dh,src,sw,sh,sw,GFX_GU_5650,alpha);
        }
        gfx_gu_clip(g_clip_x0,g_clip_y0,g_clip_x1,g_clip_y1);return;
    }
    gfx_cpu_barrier();
    unsigned short *p = fb();

    if (!p || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (alpha <= 0) return;

    for (int yy = 0; yy < dh; ++yy) {
        int py = y + yy;
        const unsigned short *srow = src + (size_t)(yy * sh / dh) * sw;

        if (py < g_clip_y0 || py >= g_clip_y1) continue;

        for (int xx = 0; xx < dw; ++xx) {
            int px = x + xx;
            unsigned short c;

            if (px < g_clip_x0 || px >= g_clip_x1) continue;
            if (!in_round_rect(xx, yy, dw, dh, r)) continue;

            c = srow[xx * sw / dw];
            if (alpha >= 255) p[py * FB_STRIDE + px] = c;
            else p[py * FB_STRIDE + px] = blend(p[py * FB_STRIDE + px], c, alpha);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 瀛楀簱                                                                */
/* ------------------------------------------------------------------ */

static const unsigned char *find_glyph(Font *f, unsigned int cp, Glyph *out)
{
    int lo = 0, hi;

    if (!f->ready) return NULL;
    hi = f->count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const unsigned char *rec = f->recs + (size_t)mid * f->rec;
        unsigned int key;

        memcpy(&key, rec, 4);
        if (key == cp) {
            out->w = rec[4];
            out->h = rec[5];
            out->bits = rec + 8;
            return rec;
        }
        if (key < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static unsigned int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned int c = *p;

    if (c < 0x80) { *s += 1; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) { *s += 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
        *s += 3;
        return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *s += 4;
        return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *s += 1;
    return '?';
}

static void draw_glyph(int x, int y, const Glyph *g, unsigned short color, int alpha)
{
    if(gu_ready()&&gfx_gu_glyph(x,y,g->w,g->h,g->bits,color,alpha))return;
    gfx_cpu_barrier();
    unsigned short *p = fb();
    int stride = (g->w + 7) >> 3;

    if (!p) return;

    for (int yy = 0; yy < g->h; ++yy) {
        int py = y + yy;
        const unsigned char *bits;

        if (py < g_clip_y0 || py >= g_clip_y1) continue;
        bits = g->bits + yy * stride;

        if (x >= g_clip_x0 && x + g->w <= g_clip_x1) {
            unsigned short *row = p + py * FB_STRIDE + x;
            for (int xx = 0; xx < g->w; ++xx) {
                if (!(bits[xx >> 3] & (0x80 >> (xx & 7)))) continue;
                if (alpha >= 255) row[xx] = color;
                else row[xx] = blend(row[xx], color, alpha);
            }
        } else {
            for (int xx = 0; xx < g->w; ++xx) {
                int px = x + xx;

                if (!(bits[xx >> 3] & (0x80 >> (xx & 7)))) continue;
                if (px < g_clip_x0 || px >= g_clip_x1) continue;
                if (alpha >= 255) p[py * FB_STRIDE + px] = color;
                else p[py * FB_STRIDE + px] = blend(p[py * FB_STRIDE + px], color, alpha);
            }
        }
    }
}

int gfx_font_h(int fs)
{
    if (fs < 0 || fs >= FS_COUNT) fs = FS_MD;
    return g_line_h[fs];
}

/* 鍙栧瓧褰細鎸夊瓧鍙烽摼閫愮骇閫€鍥烇紱杩斿洖鏃?dy 鏄瀭鐩村眳涓亸绉?*/
int gfx_theme_font_attach(int fs, const unsigned char *blob, const unsigned char *end)
{
    if (fs < 0 || fs >= FS_COUNT || !blob || !end || end <= blob) return -1;
    memset(&g_theme_fonts[fs], 0, sizeof(g_theme_fonts[fs]));
    return font_load(&g_theme_fonts[fs], blob, end, g_line_h[fs]);
}

void gfx_theme_font_clear(void)
{
    memset(g_theme_fonts, 0, sizeof(g_theme_fonts));
}

int gfx_theme_font_ready(int fs)
{
    if (fs < 0 || fs >= FS_COUNT) return 0;
    return g_theme_fonts[fs].ready;
}

static int pick_glyph(int fs, unsigned int cp, Glyph *g, int *dy)
{
    int i;

    if (fs < 0 || fs >= FS_COUNT) fs = FS_MD;

    if (g_theme_fonts[fs].ready && find_glyph(&g_theme_fonts[fs], cp, g)) {
        *dy = (g_line_h[fs] - g->h) / 2;
        return 1;
    }
    for (i = 0; i < 4 && g_chain[fs][i] >= 0; ++i) {
        int fi = g_chain[fs][i];
        if (find_glyph(&g_fonts[fi], cp, g)) {
            *dy = (g_line_h[fs] - g->h) / 2;
            return 1;
        }
    }
    for (i = 0; i < 4 && g_chain[fs][i] >= 0; ++i) {
        int fi = g_chain[fs][i];
        if (find_glyph(&g_fonts[fi], '?', g)) {
            *dy = (g_line_h[fs] - g->h) / 2;
            return 1;
        }
    }
    return 0;
}

int gfx_text_w(int fs, const char *utf8)
{
    int w = 0;
    const char *s = utf8;

    while (s && *s) {
        unsigned int cp = utf8_next(&s);
        Glyph g;
        int dy;
        if (!pick_glyph(fs, cp, &g, &dy)) continue;
        w += g.w;
    }
    return w;
}

int gfx_text_w_n(int fs, const char *utf8, int nbytes)
{
    int w = 0, used = 0;
    const char *s = utf8;

    while (s && *s && used < nbytes) {
        const char *start = s;
        unsigned int cp = utf8_next(&s);
        Glyph g;
        int dy;

        used += (int)(s - start);
        if (!pick_glyph(fs, cp, &g, &dy)) continue;
        w += g.w;
    }
    return w;
}

/* 閫氱敤缁樺埗锛歮axw<0 琛ㄧず涓嶈鍓紱ellipsis=1 鏃惰秴瀹界敾 "..." */
static int draw_run(int fs, int x, int y, const char *utf8, int maxw,
                    unsigned short color, int alpha, int ellipsis)
{
    const char *s = utf8;
    int start = x;
    int ell_w = 0;
    int limit = (maxw >= 0) ? start + maxw : 0x7FFFFFFF;

    if (ellipsis && maxw >= 0) {
        Glyph d;
        int dy;
        int i;
        for (i = 0; i < 3; ++i) {
            if (pick_glyph(fs, '.', &d, &dy)) ell_w += d.w;
        }
    }

    while (s && *s) {
        unsigned int cp = utf8_next(&s);
        Glyph g;
        int dy;

        if (!pick_glyph(fs, cp, &g, &dy)) continue;
        if (x + g.w > limit) {
            if (ellipsis) {
                int ex = x, i;
                /* 閫€鍥炲埌鑳芥斁涓?"..." 鐨勪綅缃?*/
                if (ell_w > 0 && x + ell_w > limit) ex = limit - ell_w;
                if (ex > start) {
                    for (i = 0; i < 3; ++i) {
                        Glyph d;
                        if (!pick_glyph(fs, '.', &d, &dy)) break;
                        draw_glyph(ex, y + dy, &d, color, alpha);
                        ex += d.w;
                    }
                }
            }
            return limit;
        }
        draw_glyph(x, y + dy, &g, color, alpha);
        x += g.w;
    }
    return x;
}

int gfx_text(int fs, int x, int y, const char *utf8, unsigned short color)
{
    return draw_run(fs, x, y, utf8, -1, color, 255, 0);
}

int gfx_text_a(int fs, int x, int y, const char *utf8, unsigned short color, int alpha)
{
    if (alpha >= 255) return gfx_text(fs, x, y, utf8, color);
    if (alpha <= 0) return x;
    return draw_run(fs, x, y, utf8, -1, color, alpha, 0);
}

int gfx_text_clip(int fs, int x, int y, int maxw, const char *utf8, unsigned short color)
{
    return draw_run(fs, x, y, utf8, maxw, color, 255, 0);
}

int gfx_text_ellipsis(int fs, int x, int y, int maxw, const char *utf8,
                      unsigned short color)
{
    return draw_run(fs, x, y, utf8, maxw, color, 255, 1);
}

int gfx_text_n(int fs, int x, int y, int nbytes, const char *utf8, unsigned short color)
{
    int used = 0;
    const char *s = utf8;

    while (s && *s && used < nbytes) {
        const char *start = s;
        unsigned int cp = utf8_next(&s);
        Glyph g;
        int dy;

        used += (int)(s - start);
        if (!pick_glyph(fs, cp, &g, &dy)) continue;
        draw_glyph(x, y + dy, &g, color, 255);
        x += g.w;
    }
    return x;
}

int gfx_text_right(int fs, int x_right, int y, const char *utf8, unsigned short color)
{
    return gfx_text(fs, x_right - gfx_text_w(fs, utf8), y, utf8, color);
}

int gfx_text_center(int fs, int x, int w, int y, const char *utf8, unsigned short color)
{
    return gfx_text(fs, x + (w - gfx_text_w(fs, utf8)) / 2, y, utf8, color);
}

int gfx_text_shadow(int fs, int x, int y, const char *utf8, unsigned short color,
                    unsigned short shadow)
{
    gfx_text_a(fs, x + 1, y + 1, utf8, shadow, 170);
    return gfx_text(fs, x, y, utf8, color);
}

/* ------------------------------------------------------------------ */
/* MV 缓冲上的 8888 文字                                                */
/* ------------------------------------------------------------------ */

static void mv_draw_glyph(int x, int y, const Glyph *g, unsigned int color)
{
    unsigned int *p = (unsigned int *)g_mv_vram;
    int stride = (g->w + 7) >> 3;

    if (!p) return;
    for (int yy = 0; yy < g->h; ++yy) {
        int py = y + yy;
        const unsigned char *bits;

        if (py < 0 || py >= SCR_H) continue;
        bits = g->bits + yy * stride;
        for (int xx = 0; xx < g->w; ++xx) {
            int px = x + xx;

            if (px < 0 || px >= SCR_W) continue;
            if (!(bits[xx >> 3] & (0x80 >> (xx & 7)))) continue;
            p[py * FB_STRIDE + px] = color;
        }
    }
}

void gfx_mv_text(int fs, int x, int y, const char *utf8, unsigned int color)
{
    const char *s = utf8;

    while (s && *s) {
        unsigned int cp = utf8_next(&s);
        Glyph g;
        int dy;

        if (!pick_glyph(fs, cp, &g, &dy)) continue;
        mv_draw_glyph(x, y + dy, &g, color);
        x += g.w;
    }
}

int gfx_mv_text_w(int fs, const char *utf8)
{
    const char *s = utf8;
    int w = 0;

    while (s && *s) {
        unsigned int cp = utf8_next(&s);
        Glyph g;
        int dy;

        if (!pick_glyph(fs, cp, &g, &dy)) continue;
        w += g.w;
    }
    return w;
}

/* ------------------------------------------------------------------ */
/* 鎴浘锛堣皟璇曪級/ 浜害                                                  */
/* ------------------------------------------------------------------ */

void gfx_set_brightness(int level)
{
    gfx_cpu_barrier();
    unsigned short *p = fb();

    if (!p || level >= 16) return;
    if (level < 0) level = 0;

    for (int y = 0; y < SCR_H; ++y) {
        unsigned short *row = p + y * FB_STRIDE;
        for (int x = 0; x < SCR_W; ++x) {
            unsigned short c = row[x];
            int r = (c & 0x1F) * level >> 4;
            int g = ((c >> 5) & 0x3F) * level >> 4;
            int b = ((c >> 11) & 0x1F) * level >> 4;
            row[x] = (unsigned short)(r | (g << 5) | (b << 11));
        }
    }
}

static void put32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void put16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

/* MV 缓冲（RAM 565）转 24 位 BMP：真机/模拟器上看不到画面时用它抓帧 */
int gfx_dump_mv_bmp(const char *path)
{
    const unsigned short *p = g_mv_ram;
    int row_bytes = SCR_W * 3;
    int pad = (4 - (row_bytes & 3)) & 3;
    int data_size = (row_bytes + pad) * SCR_H;
    unsigned char hdr[54];
    FILE *fp;
    unsigned char zero[4] = { 0, 0, 0, 0 };

    if (!p) return -1;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, 54 + data_size);
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, SCR_W);
    put32(hdr + 22, SCR_H);
    put16(hdr + 26, 1);
    put16(hdr + 28, 24);
    put32(hdr + 34, data_size);
    put32(hdr + 38, 2835);
    put32(hdr + 42, 2835);

    io_lock();
    fp = fopen(path, "wb");
    if (!fp) {
        io_unlock();
        return -1;
    }
    fwrite(hdr, 1, sizeof(hdr), fp);
    for (int y = SCR_H - 1; y >= 0; --y) {
        const unsigned short *row = p + y * FB_STRIDE;
        for (int x = 0; x < SCR_W; ++x) {
            unsigned short c = row[x];
            unsigned char px[3];

            /* 565 是 BGR565：B 在高 5 位、R 在低 5 位 */
            px[0] = (unsigned char)(((c >> 11) & 0x1F) << 3);  /* B */
            px[1] = (unsigned char)(((c >> 5) & 0x3F) << 2);   /* G */
            px[2] = (unsigned char)((c & 0x1F) << 3);          /* R */
            fwrite(px, 1, 3, fp);
        }
        if (pad) fwrite(zero, 1, (size_t)pad, fp);
    }
    fclose(fp);
    io_unlock();
    return 0;
}

int gfx_dump_bmp(const char *path)
{
    gfx_cpu_barrier();
    int shot_index=g_last>=0?g_last:g_back;
    unsigned short*p=(g_last>=0)?g_fb[g_last]:fb();
    if(g_renderer==GFX_RENDERER_GE&&gfx_gu_readback565(g_fb_display[shot_index],g_ram[shot_index]))p=(unsigned short*)g_ram[shot_index];
    int row_bytes = SCR_W * 3;
    int pad = (4 - (row_bytes & 3)) & 3;
    int data_size = (row_bytes + pad) * SCR_H;
    unsigned char hdr[54];
    FILE *fp;

    if (!p) return -1;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, 54 + data_size);
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, SCR_W);
    put32(hdr + 22, SCR_H);
    put16(hdr + 26, 1);
    put16(hdr + 28, 24);
    put32(hdr + 34, data_size);
    put32(hdr + 38, 2835);
    put32(hdr + 42, 2835);

    io_lock();
    fp = fopen(path, "wb");
    if (!fp) {
        io_unlock();
        return -1;
    }

    fwrite(hdr, 1, sizeof(hdr), fp);

    for (int y = SCR_H - 1; y >= 0; --y) {
        unsigned short *row = p + y * FB_STRIDE;
        for (int x = 0; x < SCR_W; ++x) {
            unsigned short c = row[x];
            unsigned char b = (unsigned char)((c >> 11) & 0x1F);
            unsigned char g = (unsigned char)((c >> 5) & 0x3F);
            unsigned char r = (unsigned char)(c & 0x1F);
            unsigned char px[3];
            px[0] = (unsigned char)((b << 3) | (b >> 2));
            px[1] = (unsigned char)((g << 2) | (g >> 4));
            px[2] = (unsigned char)((r << 3) | (r >> 2));
            fwrite(px, 1, 3, fp);
        }
        for (int i = 0; i < pad; ++i) fputc(0, fp);
    }

    fclose(fp);
    io_unlock();
    return 0;
}

int gfx_ge_page_quad(const unsigned short*src,int sw,int sh,int stride,const float xy[8],const float uv[8],unsigned short tint){if(!gu_ready())return 0;return gfx_gu_quad565(src,sw,sh,stride,xy,uv,tint);}
