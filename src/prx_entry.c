#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <unistd.h>
#include <string.h>

#include "melody_host.h"

PSP_MODULE_INFO("MelodyPSP", 0, 1, 1);
PSP_HEAP_SIZE_KB(2 * 1024);

MelodyHost *g_host;
static char launch_root[512];

extern int app_main(void);
extern void __libcglue_init(void);

static int app_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    if(launch_root[0]){if(sceIoChdir(launch_root)<0||chdir(launch_root)<0)return -1;}
    return app_main();
}

int module_start(SceSize argc, void *argp)
{
    MelodyHost **pp = (MelodyHost **)argp;
    SceUID th;

    __libcglue_init();
    if (argc >= (SceSize)sizeof(MelodyHost *) && pp) g_host = *pp;
    if(argc>=sizeof(MelodyHost*)+sizeof(launch_root)&&argp){memcpy(launch_root,(char*)argp+sizeof(MelodyHost*),sizeof(launch_root));launch_root[sizeof(launch_root)-1]=0;}
    th = sceKernelCreateThread("melody_main", app_thread, 0x20, 0x40000,
                               PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return 0;
}

int module_stop(SceSize argc, void *argp)
{
    (void)argc;
    (void)argp;
    return 0;
}

void _exit(int code)
{
    (void)code;
    for (;;) { }
}
