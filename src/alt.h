/*
 * MELODY PSP — 备用音源（免费资源回退）
 * ------------------------------------------------------------------
 * 网易官方接口对 VIP 歌只给 45 秒试听，或者干脆不给地址。这里按顺序尝试
 * 其它公开音源，拿到一个能直接播放的 HTTP mp3 直链。
 *
 * 所有实现都在 alt.c；设置持久化在 ms0:/PSP/GAME/MELODY/alt.txt。
 * 只应在 api 工作线程里调用（内部会发 HTTP 请求）。
 */

#ifndef MELODY_ALT_H
#define MELODY_ALT_H

#include "app.h"

enum {
    ALT_BYFUNS = 0,    /* byfuns：纯 HTTP 纯文本接口，直接回 mp3 直链（最省事） */
    ALT_KUWO,          /* 酷我：纯 HTTP，免鉴权，独立曲库 */
    ALT_GATEWAY,       /* 局域网网关（UnblockNeteaseMusic / NCM 反代） */
    ALT_GDSTUDIO,      /* GDStudio：本身只有 HTTPS，需要网关或 TLS */
    ALT_SOURCE_COUNT
};

void alt_init(void);
void alt_save(void);

const char *alt_name(int i);       /* "酷我" / "本地网关" / "GDStudio" */
const char *alt_desc(int i);       /* 设置页的一行说明 */
int  alt_enabled(int i);
void alt_set_enabled(int i, int on);

const char *alt_gateway(void);     /* "192.168.1.10:8088"，空 = 没配 */
void alt_set_gateway(const char *hp);

/*
 * 从备用源解析一个可播放直链。
 *   成功：返回 1，url 填成 http:// 开头的 mp3 直链，src 写用了哪个源；
 *   失败：返回 0（alt_last_error() 里有原因）。
 * 内部会跳过未启用的源；顺序：网关 → 酷我 → GDStudio（网关配了才走 GDStudio）。
 */
int alt_resolve(const Song *song, char *url, int urllen, char *src, int srclen);

const char *alt_last_error(void);
/* 上一次 alt_resolve 成功时校验到的直链字节数（0 = 不知道）。下载进度要用。 */
int alt_last_size(void);

/* 第三方搜索结果源（酷我，纯 HTTP）：把原唱条目补进搜索列表。
 * 只在 API worker 上调用；返回写入条数（0 = 未启用/失败/无结果）。
 * 返回的 Song 带 playable=2、fee=-1（外源标记），播放与下载走备用音源。 */
int alt_kuwo_search_songs(const char *query, Song *out, int max);

#endif
