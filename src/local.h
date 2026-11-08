/*
 * MELODY PSP - 本地音乐与导出
 */

#ifndef MELODY_LOCAL_H
#define MELODY_LOCAL_H

#include "app.h"

/* 递归扫描 ms0:/MUSIC 与 ms0:/PSP/MUSIC，MELODY 子树优先且只遍历一次。
 * 最多 256 首，完整路径不区分大小写去重；路径含 NUL 最多 256 字节。
 * 同时打开最多 32 层目录（含根）；超长路径、超深子树与读取失败项跳过。
 * 结果缓存到 local_rescan()；返回实际收录数，达到容量后停止扫描。 */
int local_scan(void);
void local_rescan(void);

int local_count(void);
const char *local_path_of(int i);   /* 完整路径 */
const char *local_name(int i);      /* 文件名 */
int local_size(int i);

/* 把缓存里的歌导出到 ms0:/MUSIC（带 ID3v2.3），同步执行（放在 api 线程里跑） */
int local_export(const Song *s);    /* 0 成功，-1 无缓存，-2 空间不足，-3 写失败 */
int local_export_pct(void);
int local_export_active(void);   /* 1 = 正在导出 */
int local_export_exists(long long id);   /* 1 = 已经导出过 */
int local_export_result(void);      /* -1 = 没跑过 */
const char *local_export_path(void);
void local_export_reset(void);

#endif
