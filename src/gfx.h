/*
 * MELODY PSP — 软件渲染器
 * ------------------------------------------------------------------
 * 480x272，16bpp (565)，在普通内存里画（走 D-cache），每帧一次 memcpy
 * 到 VRAM 非缓存别名，再翻转。所有绘制都是 CPU 直接写 framebuffer。
 *
 * 坐标一律是屏幕坐标，越界自动裁掉；另有一套可设置的裁剪矩形。
 *
 * 四套字库（tools/make_font.py 生成，见 font_data.S）：
 *   FS_SM  12px  GB2312（时间/歌手/标签）
 *   FS_MD  16px  全 GBK（正文/列表标题）
 *   FS_LG  22px  GB2312（标题；缺字自动退回 16px）
 *   FS_XL  32px  ASCII（大数字/英文，中文自动退回 LG）
 */

#ifndef MELODY_GFX_H
#define MELODY_GFX_H

#define SCR_W 480
#define SCR_H 272
#define FB_STRIDE 512

/* 字号 */
enum { FS_SM = 0, FS_MD = 1, FS_LG = 2, FS_XL = 3 };
#define FS_COUNT 4

enum{GFX_RENDERER_CPU=0,GFX_RENDERER_GE=1};
int gfx_set_renderer(int renderer);
int gfx_renderer(void);
const char*gfx_renderer_error(void);
void gfx_cpu_barrier(void);
void gfx_suspend_renderer(void);
void gfx_resume_renderer(void);
/* External page transforms can opt into an accelerated captured-page quad. */
int gfx_ge_page_quad(const unsigned short*src,int sw,int sh,int stride,const float xy[8],const float uv[8],unsigned short tint);
unsigned short gfx_rgb(int r, int g, int b);
unsigned short gfx_lerp565(unsigned short a, unsigned short b, int t); /* t:0..256 */

void gfx_init(void);
void gfx_set_present_overlay(void (*draw)(void));
void gfx_flip(void);
void gfx_set_brightness(int level); /* 0..16，整屏压暗（转场用） */

/* ---- 基础图元 ---- */
void gfx_clear(unsigned short color);
void gfx_pixel(int x, int y, unsigned short color);
void gfx_fill_rect(int x, int y, int w, int h, unsigned short color);
void gfx_fill_rect_a(int x, int y, int w, int h, unsigned short color, int alpha);
void gfx_hline(int x, int y, int w, unsigned short color);
void gfx_vline(int x, int y, int h, unsigned short color);

/* 圆角矩形（半径最大 32） */
void gfx_fill_round(int x, int y, int w, int h, int r, unsigned short color);
void gfx_fill_round_a(int x, int y, int w, int h, int r, unsigned short color, int alpha);
void gfx_round_border(int x, int y, int w, int h, int r, unsigned short color);
void gfx_round_border_a(int x, int y, int w, int h, int r, unsigned short color, int alpha);

/* 圆角 + 竖直渐变（卡片用） */
void gfx_fill_round_grad(int x, int y, int w, int h, int r,
                         unsigned short top, unsigned short bottom);

/* 渐变：竖直/水平方向线性插值 */
void gfx_vgrad(int x, int y, int w, int h, unsigned short top, unsigned short bottom);
void gfx_hgrad(int x, int y, int w, int h, unsigned short left, unsigned short right);

/* 圆 / 圆环 / 圆弧（角度：0=右，顺时针，度数） */
void gfx_circle(int cx, int cy, int r, unsigned short color);
void gfx_circle_a(int cx, int cy, int r, unsigned short color, int alpha);
void gfx_ring(int cx, int cy, int r_out, int r_in, unsigned short color);
void gfx_ring_a(int cx, int cy, int r_out, int r_in, unsigned short color, int alpha);
void gfx_arc(int cx, int cy, int r_out, int r_in, int a0, int a1,
             unsigned short color, int alpha);

/* 三角形（图标用），alpha<255 时混合 */
void gfx_tri(int x0, int y0, int x1, int y1, int x2, int y2,
             unsigned short color, int alpha);

/* ---- 位图 ---- */
void gfx_blit565(int x, int y, int w, int h, const unsigned short *src, int stride);
void gfx_blit565_a(int x, int y, int w, int h, const unsigned short *src, int stride, int alpha);
void gfx_blit565_scale(int x, int y, int dw, int dh,
                       const unsigned short *src, int sw, int sh);
/* 缩放 + 圆角遮罩（封面用，四角透明，不会糊掉背景） */
void gfx_blit565_round(int x, int y, int dw, int dh,
                       const unsigned short *src, int sw, int sh, int r, int alpha);

/* ---- 裁剪 ---- */
void gfx_set_clip(int x0, int y0, int x1, int y1);
void gfx_reset_clip(void);
int gfx_push_clip(int x0,int y0,int x1,int y1);
void gfx_pop_clip(void);
void gfx_blit_rgba_quad(const unsigned char*src,int sw,int sh,const float xy[8],const float uv[8],int alpha);
void gfx_blit_rgba_rotated(int x,int y,int size,const unsigned char*src,int sw,int sh,float angle);
void gfx_blit_rgba_scaled(int x,int y,int w,int h,const unsigned char*rgba,int sw,int sh,int alpha);
void gfx_blit565_scaled(int x,int y,int w,int h,const unsigned short*src,int sw,int sh);
void gfx_blit565_cover(int x,int y,int w,int h,const unsigned short*src,int sw,int sh);
void gfx_get_clip(int *x0, int *y0, int *x1, int *y1);

/* ---- 文字（UTF-8） ----
 * 字号用 FS_*；返回这行画完后的 x（右边界），方便接着画。
 * 所有函数的 y 都是「文字格子顶部」：12px 字占 y..y+12，16px 占 y..y+16。 */
void gfx_text_init(void);
int gfx_font_h(int fs);
/* 主题自带字体：挂载后该字号优先使用它，缺字回退内置字库。
 * blob 必须指向 PSPF v1 字库（校验由 skin_font.c 负责），生命周期由调用方管理。 */
int gfx_theme_font_attach(int fs, const unsigned char *blob, const unsigned char *end);
void gfx_theme_font_clear(void);
int gfx_theme_font_ready(int fs);

int gfx_text_w(int fs, const char *utf8);
int gfx_text(int fs, int x, int y, const char *utf8, unsigned short color);
int gfx_text_a(int fs, int x, int y, const char *utf8, unsigned short color, int alpha);
int gfx_text_clip(int fs, int x, int y, int maxw, const char *utf8, unsigned short color);
int gfx_text_ellipsis(int fs, int x, int y, int maxw, const char *utf8,
                      unsigned short color);
int gfx_text_n(int fs, int x, int y, int nbytes, const char *utf8, unsigned short color);
int gfx_text_w_n(int fs, const char *utf8, int nbytes);
int gfx_text_right(int fs, int x_right, int y, const char *utf8, unsigned short color);
int gfx_text_center(int fs, int x, int w, int y, const char *utf8, unsigned short color);
/* 带 1px 投影，暗背景下更清楚 */
int gfx_text_shadow(int fs, int x, int y, const char *utf8, unsigned short color,
                    unsigned short shadow);

/* ---- 调试 ---- */
int gfx_dump_bmp(const char *path);
unsigned short *gfx_backbuffer(void);
unsigned short *gfx_vram_backbuffer(void);
void gfx_set_skip_copy(int skip);
void gfx_set_immediate(int immediate);

/* ---- MV 专用显示 ----
 * MV 不再用 CPU 逐像素写 VRAM（那是撕裂和卡死的根源），而是：
 *   1. 启动时把 scanout 切到一块独立的 512x272 RGBA8888 单缓冲；
 *   2. 每帧用 sceDmacMemcpy 把解码器的 RGB 帧整块搬进去（DMA 不过 D-cache）；
 *   3. OSD 直接画在同一个缓冲上，下一帧 DMA 会盖掉，所以每帧重画。
 * 完全不调用 sceDisplaySetFrameBuf 换页。 */
void gfx_begin_direct_video(void);
void gfx_end_direct_video(void);
unsigned short *gfx_mv_ram(void);      /* RAM 里的 565 视频帧（512x272），CPU 写这里 */
void *gfx_mv_vram(void);               /* 显存非缓存别名（565），DMA 目标 */
/* 注意：旧版 8888 OSD 绘制（gfx_mv_rgb/fill/text）已停用并从公开接口移除；
 * 565@offset 0 下它们会写进正在扫描的 UI buffer，不要再调用。 */
void gfx_mv_set_osd(int on);           /* 565 路径下无 OSD（保留接口） */
int gfx_mv_osd(void);
int gfx_dump_mv_bmp(const char *path);   /* 调试：把当前 MV 帧存成 BMP */

#endif
