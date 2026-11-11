#include "wifi_dialog.h"
#include "net.h"

#include <psputility.h>
#include <psputility_netconf.h>
#include <pspnet_apctl.h>
#include <string.h>

/* Kept explicit while the integrating owner adds these coordination contracts
 * to net.h. begin reserves network control even when returning pending (0).
 * end must accept both a pending reservation and an acquired reservation. */
extern int net_offline(void);
extern int net_dialog_begin(void);
extern void net_dialog_end(int connected);

static pspUtilityNetconfData g_params;
static WifiDialogState g_state;
static WifiDialogResult g_result;
static int g_error;
static int g_reserved;
static int g_shutdown_started;
static int g_offline_seen;
static int g_sdk_status;

int wifi_dialog_blocks_input(void)
{
    return g_reserved;
}

WifiDialogState wifi_dialog_get_state(void) { return g_state; }
WifiDialogResult wifi_dialog_get_result(void) { return g_result; }
int wifi_dialog_error(void) { return g_error; }

static void finish(int error)
{
    int connected = !error && !g_offline_seen && !net_offline() &&
                    g_params.base.result == 0 &&
                    net_apctl_state() == PSP_NET_APCTL_STATE_GOT_IP;
    g_error = error ? error : g_params.base.result;
    g_result = connected ? WIFI_DIALOG_RESULT_CONNECTED :
               g_error < 0 ? WIFI_DIALOG_RESULT_ERROR : WIFI_DIALOG_RESULT_CANCELLED;
    g_state = g_result == WIFI_DIALOG_RESULT_ERROR ? WIFI_DIALOG_FAILED : WIFI_DIALOG_DONE;
    net_dialog_end(connected);
    g_reserved = 0;
}

int wifi_dialog_start(void)
{
    if (g_reserved) return -1;
    if (net_offline()) return -2;
    memset(&g_params, 0, sizeof(g_params));
    g_params.base.size = sizeof(g_params);
    /* Match PSPSDK utility samples. System preferences retain region-specific
     * confirm/cancel buttons; English/circle defaults survive a failed query. */
    g_params.base.language = 1;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &g_params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &g_params.base.buttonSwap);
    g_params.base.graphicsThread = 17;
    g_params.base.accessThread = 19;
    g_params.base.fontThread = 18;
    g_params.base.soundThread = 16;
    g_params.action = PSP_NETCONF_ACTION_CONNECTAP;
    g_state = WIFI_DIALOG_WAITING;
    g_result = WIFI_DIALOG_RESULT_NONE;
    g_error = g_shutdown_started = g_offline_seen = 0;
    g_sdk_status = PSP_UTILITY_DIALOG_NONE;
    g_reserved = 1;
    return 0;
}

void wifi_dialog_tick(void)
{
    int result;
    if (!g_reserved) return;
    if (net_offline()) g_offline_seen = 1;
    if (g_state == WIFI_DIALOG_WAITING) {
        if (g_offline_seen) { finish(-2); return; }
        result = net_dialog_begin();
        if (result < 0) { finish(result); return; }
        if (result == 0) return;
        result = sceUtilityNetconfInitStart(&g_params);
        if (result < 0) { finish(result); return; }
        g_state = WIFI_DIALOG_ACTIVE;
        /* InitStart is asynchronous; observe INIT/NONE on the next tick. */
        return;
    }
    g_sdk_status = sceUtilityNetconfGetStatus();
    if (g_sdk_status == PSP_UTILITY_DIALOG_NONE) {
        finish(g_error);
    } else if (g_sdk_status == PSP_UTILITY_DIALOG_QUIT && !g_shutdown_started) {
        result = sceUtilityNetconfShutdownStart();
        if (result >= 0) {
            g_shutdown_started = 1;
            g_state = WIFI_DIALOG_SHUTDOWN;
        }
        /* A transient shutdown error must retain ownership and retry at QUIT.
         * Do not release while utility helper threads may still access data. */
    } else if (g_sdk_status < 0) {
        /* Retain storage, ownership and input exclusion until confirmed NONE.
         * A status-query error does not prove the SDK has destroyed its dialog. */
        g_error = g_sdk_status;
    }
}

void wifi_dialog_draw(void)
{
    int result;
    if (!g_reserved || g_state != WIFI_DIALOG_ACTIVE ||
        g_sdk_status != PSP_UTILITY_DIALOG_VISIBLE) return;
    result = sceUtilityNetconfUpdate(1);
    if (result < 0) g_error = result;
}
