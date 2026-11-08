/* Frame-capture test: decode ms0:/PSP/GAME/MVTest/mv.mp4 and dump presented
 * frames as binary PPM (P6) so they can be compared with the original video on
 * PC. Built with the same decoder flags as the shipping CPU renderer.
 *
 * Optional config file ms0:/PSP/GAME/MVTest/capcfg.txt:
 *     <start> <count> <step>
 * capture the presented frames start, start+step, ... (count of them).
 * Defaults: 400 1 1  (frame 400 = the animation segment, ~29-30 s at 25 fps;
 * PPSSPP presents ~13 fps, so presented index 400 lands around 29 s of PTS).
 *
 * Outputs (all under ms0:/PSP/GAME/MVTest):
 *     frame.ppm            when count == 1
 *     frame_<seen>.ppm     when count > 1
 *     cap.txt              header line + one line per capture (ms/seen/flags)
 *     result.txt           decode result + build flags
 * The compile-time switch values are written into cap.txt/result.txt so a build
 * whose flags did not take effect is visible without trusting the build log. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "psp_video.h"

PSP_MODULE_INFO("MVFrame", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(12 * 1024);

#ifndef PSP_SKIP_LOOP_FILTER
#define PSP_SKIP_LOOP_FILTER -1
#endif
#ifndef PSP_APPROX_QPEL
#define PSP_APPROX_QPEL -1
#endif
#ifndef PSP_PACKED_CHROMA
#define PSP_PACKED_CHROMA -1
#endif
#ifndef PSP_NEAREST_CHROMA
#define PSP_NEAREST_CHROMA -1
#endif
#ifndef PSP_RENDER_NATIVE
#define PSP_RENDER_NATIVE -1
#endif
#ifndef PSP_ME_RENDER
#define PSP_ME_RENDER -1
#endif
#ifndef PSP_AUDIO
#define PSP_AUDIO -1
#endif
#ifndef PSP_BUILD_ID
#define PSP_BUILD_ID "?"
#endif

#define CAP_DIR "ms0:/PSP/GAME/MVTest"

static int g_start = 400;        /* first presented frame to capture */
static int g_count = 1;          /* how many captures */
static int g_step = 1;           /* presented-frame stride */
static int g_target_ms;          /* >0: 按 PTS 抓（第 1 个 position_ms >= 它的帧） */
static int g_seen;
static int g_captured;
static int g_cap_ms[64];
static int g_cap_seen[64];

static void flags_line(char *b, int n)
{
    snprintf(b, n, "flags slf=%d qpel=%d packed_chroma=%d nearest_chroma=%d "
                  "render_native=%d me=%d audio=%d build=%s",
             PSP_SKIP_LOOP_FILTER, PSP_APPROX_QPEL, PSP_PACKED_CHROMA,
             PSP_NEAREST_CHROMA, PSP_RENDER_NATIVE, PSP_ME_RENDER, PSP_AUDIO,
             PSP_BUILD_ID);
}

static void append_line(const char *path, const char *line)
{
    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    int n = (int)strlen(line);

    if (fd < 0) return;
    sceIoWrite(fd, line, n);
    sceIoClose(fd);
}

static void ppm_write(const uint32_t *pixels, int w, int h, const char *path)
{
    static unsigned char row[512 * 3];
    char hdr[64];
    int fd, n;

    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    n = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", w, h);
    sceIoWrite(fd, hdr, n);
    for (int y = 0; y < h; ++y) {
        const uint32_t *src = pixels + (y + 1) * 512;   /* visible area starts at row 1 */
        int m = 0;

        for (int x = 0; x < w; ++x) {
#if PSP_OUTPUT565
            unsigned c = ((const uint16_t *)src)[x];
            unsigned r=c&31, g=(c>>5)&63, b=(c>>11)&31;
#else
            uint32_t c = src[x];
            unsigned r=((c&255)+4)>>3, g=(((c>>8)&255)+2)>>2, b=(((c>>16)&255)+4)>>3;
            if(r>31)r=31;
            if(g>63)g=63;
            if(b>31)b=31;
#endif
            row[m++] = (unsigned char)((r<<3)|(r>>2));
            row[m++] = (unsigned char)((g<<2)|(g>>4));
            row[m++] = (unsigned char)((b<<3)|(b>>2));
        }
        sceIoWrite(fd, row, m);
    }
    sceIoClose(fd);
}

static int present(void *opaque, const uint32_t *pixels, const PspVideoStatus *st)
{
    char path[96];
    char line[192];
    int index;

    (void)opaque;
    if (!pixels) return 0;
    ++g_seen;
    if (g_captured >= g_count) return 0;
    if (g_target_ms > 0) {
        /* 按 PTS 抓：不同文件/不同解码速度下都落在同一源帧 */
        if ((int)st->position_ms < g_target_ms) return 0;
    } else {
        if (g_seen < g_start) return 0;
        if ((g_seen - g_start) % g_step != 0) return 0;
    }
    index = g_captured;
    if (index >= (int)(sizeof(g_cap_seen) / sizeof(g_cap_seen[0]))) return 0;

    if (g_count == 1)
        snprintf(path, sizeof(path), CAP_DIR "/frame.ppm");
    else
        snprintf(path, sizeof(path), CAP_DIR "/frame_%04d.ppm", g_seen);
    ppm_write(pixels, 480, 270, path);

    g_cap_ms[index] = (int)st->position_ms;
    g_cap_seen[index] = g_seen;
    snprintf(line, sizeof(line), "cap idx=%d seen=%d ms=%d dur=%d file=%s\n",
             index, g_seen, (int)st->position_ms, (int)st->duration_ms,
             strrchr(path, '/') + 1);
    append_line(CAP_DIR "/cap.txt", line);
    ++g_captured;
    if (g_captured >= g_count) psp_video_stop();
    return 0;
}

static const PspVideoPresenter g_pres = { NULL, present, NULL };

/* <start> <count> <step> [<target_ms>]；缺省/短文件保留默认值。
 * target_ms > 0 时忽略 start/step，改为“第 1 个 position_ms >= target_ms 的帧”，
 * 这样原片和转码片（解码速度不同）能抓到同一 PTS。 */
static void read_cfg(void)
{
    int fd = sceIoOpen(CAP_DIR "/capcfg.txt", PSP_O_RDONLY, 0);
    char b[64];
    int n;

    if (fd < 0) return;
    n = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    b[n] = 0;
    if (sscanf(b, "%d %d %d %d", &g_start, &g_count, &g_step, &g_target_ms) < 1) return;
    if (g_start < 1) g_start = 1;
    if (g_count < 1) g_count = 1;
    if (g_count > 64) g_count = 64;
    if (g_step < 1) g_step = 1;
    if (g_target_ms < 0) g_target_ms = 0;
}

int main(void)
{
    PspVideoOptions opt;
    PspVideoResult res;
    char b[320];
    char flags[192];
    int r, fd, n;

    sceIoMkdir(CAP_DIR, 0777);
    read_cfg();
    flags_line(flags, sizeof(flags));

    /* cap.txt: 头行记录本次配置与编译期开关，后面每抓一帧追加一行 */
    fd = sceIoOpen(CAP_DIR "/cap.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        n = snprintf(b, sizeof(b), "cfg start=%d count=%d step=%d target_ms=%d\n%s\n",
                     g_start, g_count, g_step, g_target_ms, flags);
        sceIoWrite(fd, b, n);
        sceIoClose(fd);
    }

    memset(&opt, 0, sizeof(opt));
    memset(&res, 0, sizeof(res));
    opt.path = CAP_DIR "/mv.mp4";
    opt.log_root = CAP_DIR;
    opt.backend = PSP_VIDEO_BACKEND_NATIVE_ME;
    opt.rgb_buffer_frames = 2;
    /* 0 = 完整解码。非 0 时 player.c 内部仍按 SAMPLE_FRAMES=300 停（实测 330 个
     * 呈现帧），抓第 420 帧够不到，所以这里必须走完整模式，抓完由 PC 侧 kill。 */
    opt.sample = 0;
#if PSP_DECODER_PROFILE
    opt.diagnostic_profile = 1;
#endif
    opt.cancel_button = PSP_VIDEO_CANCEL_CIRCLE;
    opt.presenter = &g_pres;

    r = psp_video_play(&opt, &res);

    fd = sceIoOpen(CAP_DIR "/result.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    n = snprintf(b, sizeof(b), "r=%d frames=%u pos=%lld captured=%d seen=%d\n%s\n",
                 r, res.frames, (long long)res.position_ms, g_captured, g_seen, flags);
    if (fd >= 0) { sceIoWrite(fd, b, n); sceIoClose(fd); }

    sceKernelExitGame();
    return 0;
}
