/*
 * MELODY PSP - 系统信息（内存 / 记忆棒剩余空间）
 * ------------------------------------------------------------------
 * 每 2 秒刷新一次并缓存：sceIoDevctl 每次都问 FAT 太贵，不能每帧调。
 */

#ifndef MELODY_SYSINFO_H
#define MELODY_SYSINFO_H

void sysi_update(void);          /* 主循环每 2 秒调一次 */

int sysi_free_kb(void);          /* 系统空闲内存（KB） */
int sysi_maxblock_kb(void);      /* 最大连续块（KB） */
int sysi_used_kb(void);          /* 已占用 = 32MB - 空闲（含程序、堆、显存镜像） */
int sysi_ms0_free_mb(void);      /* 记忆棒剩余（MB），-1 = 未知 */
int sysi_ms0_total_mb(void);     /* 记忆棒容量（MB） */

#endif
