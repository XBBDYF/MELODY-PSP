#ifndef MELODY_HOST_H
#define MELODY_HOST_H

#include "psp_video.h"

/* Loader-owned host API handed to melody.prx at module start. */
typedef struct {
    int  (*mv_load)(void);
    int  (*mv_play)(const PspVideoOptions *opt, PspVideoResult *res);
    void (*mv_stop)(void);
    int  (*mv_unload)(void); /* 0 only after confirmed retirement; deploy with paired EBOOT. */
} MelodyHost;

extern MelodyHost *g_host;

#endif
