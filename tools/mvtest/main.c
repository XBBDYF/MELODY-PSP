/* Minimal MV decoder smoke test (CPU-only, WiliMini pspvideo).
 * Plays ms0:/PSP/GAME/MVTest/mv.mp4 and writes result.txt.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "psp_video.h"

PSP_MODULE_INFO("MVTest", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(12 * 1024);

int main(void)
{
    PspVideoOptions opt;
    PspVideoResult res;
    int r;
    int fd;
    char b[256];
    int n;

    sceIoMkdir("ms0:/PSP/GAME/MVTest", 0777);

    memset(&opt, 0, sizeof(opt));
    memset(&res, 0, sizeof(res));
    opt.path = "ms0:/PSP/GAME/MVTest/mv.mp4";
    opt.log_root = "ms0:/PSP/GAME/MVTest";
    opt.backend = PSP_VIDEO_BACKEND_NATIVE_ME;
    opt.rgb_buffer_frames = 2;
    opt.sample = 120;            /* quick smoke test: ~120 decoded frames */
    opt.cancel_button = PSP_VIDEO_CANCEL_CIRCLE;

    r = psp_video_play(&opt, &res);

    fd = sceIoOpen("ms0:/PSP/GAME/MVTest/result.txt",
                   PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    n = snprintf(b, sizeof(b),
                 "r=%d error=%d stage=%d cancelled=%d frames=%u eof=%u pos=%lld\n",
                 r, res.error, (int)res.error_stage, res.cancelled, res.frames, res.eof,
                 (long long)res.position_ms);
    if (fd >= 0) {
        sceIoWrite(fd, b, n);
        sceIoClose(fd);
    }

    sceKernelExitGame();
    return 0;
}
