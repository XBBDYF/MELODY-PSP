/*
 * MELODY PSP - 系统信息（内存 / 记忆棒空间）
 */

#include "sysinfo.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>

typedef struct {
    int max_clusters;
    int free_clusters;
    int max_freesize;
    int free_size;
} MsInfo;

static int g_free_kb;
static int g_maxblock_kb;
static int g_ms0_free_mb = -1;
static int g_ms0_total_mb = -1;
static int g_inited;

void sysi_update(void)
{
    MsInfo info;

    g_free_kb = (int)(sceKernelTotalFreeMemSize() / 1024);
    g_maxblock_kb = (int)(sceKernelMaxFreeMemSize() / 1024);

    memset(&info, 0, sizeof(info));
    if (sceIoDevctl("ms0:", 0x02425818, &info, sizeof(info), NULL, 0) >= 0 &&
        info.max_clusters > 0 && info.free_size > 0) {
        g_ms0_free_mb = info.free_size / (1024 * 1024);
        g_ms0_total_mb = (info.max_clusters * 32) / 1024;   /* 簇大小 32KB */
        if (g_ms0_total_mb <= 0) g_ms0_total_mb = -1;
    } else {
        /* 模拟器不一定实现这个 devctl：当作「未知」，别当成 0 */
        g_ms0_free_mb = -1;
        g_ms0_total_mb = -1;
    }
    g_inited = 1;
}

int sysi_free_kb(void) { return g_free_kb; }
int sysi_maxblock_kb(void) { return g_maxblock_kb; }
int sysi_used_kb(void)
{
    int used = (int)(32 * 1024) - g_free_kb;

    return used > 0 ? used : 0;   /* 模拟器会报比 32MB 还多的空闲 */
}
int sysi_ms0_free_mb(void) { return g_inited ? g_ms0_free_mb : -1; }
int sysi_ms0_total_mb(void) { return g_inited ? g_ms0_total_mb : -1; }
