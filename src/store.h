/*
 * MELODY PSP — 最近播放 / 收藏（纯文本，存在记忆棒里）
 */

#ifndef MELODY_STORE_H
#define MELODY_STORE_H

#include "app.h"

void store_init(void);
void store_save(void);
int store_playlist_has(long long id);
int store_playlist_toggle(const Card *card);
int store_playlist_snapshot(Card *out,int max);

int store_recent_count(void);
const Song *store_recent_get(int i);
const Song *store_recent(int i);
void store_push_recent(const Song *s);
void store_clear_recent(void);

int store_fav_count(void);
const Song *store_fav(int i);
int store_is_fav(long long id);

/* 音质：0 标准128k / 1 较高192k / 2 极高320k */
int store_quality(void);
void store_set_quality(int q);

/* 右下角内存显示（debug） */
int store_show_speed(void);
void store_set_show_speed(int on);
int store_show_mem(void);
void store_set_show_mem(int on);

/* 收藏 / 取消收藏；返回 0 = 已添加，1 = 已移除，2 = 收藏已满（未添加） */
int store_toggle_fav(const Song *s);

/* 主题：0 = 浅色（默认） / 1 = 深色 */
int store_theme(void);
void store_set_theme(int theme);

/* 全局界面渲染：0 = CPU 软件（默认），1 = GE / GU；独立 E 行持久化。
 * setter 保存用户选择；实际切换由 gfx 在帧边界执行。 */
int store_renderer(void);
void store_set_renderer(int renderer);

/* 唱片播放器：0 = 原方形封面（默认），1 = 旋转唱片；独立 V 行。 */
int store_disc_player(void);
void store_set_disc_player(int on);

/* 本地 MV：1 = 额外扫描 ms0:/VIDEO 与 ms0:/PSP/VIDEO，0 = 只显示本软件下载的 MV（默认）。
 * 独立 L 行；默认关闭，UI 打开时要提示不推荐。 */
int store_scan_local_mv(void);
void store_set_scan_local_mv(int on);

/* 下载并发连接数（2..6，默认 4，和 dl.c 的 DL_MAX_PARTS 一致） */
int store_dl_parts(void);
void store_set_dl_parts(int parts);

int store_prefetch_count(void);
void store_set_prefetch_count(int n);
#endif
