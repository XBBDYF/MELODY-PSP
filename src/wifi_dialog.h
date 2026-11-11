/* Cooperative PSP system network configuration dialog; main/render thread only. */
#ifndef MELODY_WIFI_DIALOG_H
#define MELODY_WIFI_DIALOG_H

typedef enum {
    WIFI_DIALOG_IDLE = 0,
    WIFI_DIALOG_WAITING,
    WIFI_DIALOG_ACTIVE,
    WIFI_DIALOG_SHUTDOWN,
    WIFI_DIALOG_DONE,
    WIFI_DIALOG_FAILED
} WifiDialogState;

typedef enum {
    WIFI_DIALOG_RESULT_NONE = 0,
    WIFI_DIALOG_RESULT_CONNECTED = 1,
    WIFI_DIALOG_RESULT_CANCELLED = 2,
    WIFI_DIALOG_RESULT_ERROR = -1
} WifiDialogResult;

/* 0: accepted, -1: already active, -2: offline. Nonblocking.
 * Caller first pauses API/download scheduling and cancels their in-flight jobs.
 * start/tick/draw/getters must all run on the SAME main/render thread. */
int wifi_dialog_start(void);
/* Once per frame before rendering. Waits cooperatively for net_dialog_begin,
 * starts utility, observes state and requests shutdown when QUIT is reported. */
void wifi_dialog_tick(void);
/* Once per rendered frame AFTER app GU list finish/sync, BEFORE vblank/swap.
 * Calls sceUtilityNetconfUpdate(1) only in VISIBLE. The caller must keep a
 * valid 480x272 framebuffer/GU setup (including with the CPU drawing backend).
 * Never nest this inside an open application GU display list. */
void wifi_dialog_draw(void);
/* Suppress ALL application input from accepted start through utility NONE.
 * The utility reads PSP controls itself; do not synthesize cancel/confirm.
 * After release, flush held/repeat buttons before restoring application input. */
int wifi_dialog_blocks_input(void);
WifiDialogState wifi_dialog_get_state(void);
WifiDialogResult wifi_dialog_get_result(void);
/* SDK result/error, retained until next accepted start. */
int wifi_dialog_error(void);

#endif
