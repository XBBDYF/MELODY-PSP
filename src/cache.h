/*
 * MELODY PSP — 歌曲缓存（下载到记忆棒里的歌）
 * ------------------------------------------------------------------
 * 目录：
 *   ms0:/PSP/GAME/MELODY/cache/<id>.mp3    整首 mp3（下载中也是这个名字）
 *   ms0:/PSP/GAME/MELODY/cache/<id>.ok     完成标记（空文件）
 *   ms0:/PSP/GAME/MELODY/cache/<id>.jpg    封面（离线也能看）
 *   ms0:/PSP/GAME/MELODY/cache/index.txt   索引（纯文本，一行一首）
 *
 * 只有带 .ok 的文件才算「已缓存」：程序被硬关机时残留的半截文件，
 * 下次启动保留以便续传，但不会列入已完成缓存。
 */

#ifndef MELODY_CACHE_H
#define MELODY_CACHE_H

#include "app.h"

#define CACHE_MAX 64 /* legacy snapshot window; do not use as disk capacity */
#define CACHE_DISK_INDEX 1 /* no fixed song capacity; bounded resident lookup cache */

typedef struct {
    long long id;
    int dur_ms;
    int bytes;
    char name[72];
    char artist[72];
    char album[44];
    char art[208];
} CacheEntry;

void cache_init(void);
void cache_save(void);

int cache_count(void);
/* Returned metadata lives in an 8-slot ring: copy promptly; snapshots own data. */
const CacheEntry *cache_get(int i);
const CacheEntry *cache_find(long long id);

/* 线程安全的快照：dl 线程会往里加歌，UI 只读这个副本 */
int cache_snapshot(CacheEntry *out, int max);
/* first is a zero-based absolute index; returns entries copied, not total count. */
int cache_snapshot_page(CacheEntry *out, int max, int first);
/* Write generation-consistent native 64-bit IDs in cache order. Returns song
 * count, or -1 on failure (previous destination retained). Caller owns path. */
int cache_snapshot_ids(const char *path);

/* 线程安全的文件路径：调用方自己给缓冲。
 * kind: 0 = .mp3 / 1 = .ok / 2 = .jpg / 3 = .lrc */
void cache_path(char *out, int n, int kind, long long id);

/* 旧接口（内部 ring buffer，返回的指针要立刻用掉，不要跨线程持有） */
const char *cache_file(long long id);
const char *cache_ok_file(long long id);
const char *cache_art_file(long long id);

int cache_file_bytes(long long id);     /* -1 = 文件不在 */
int cache_complete(long long id);       /* 索引里有 + 文件在 */
int cache_has(long long id);            /* 线程安全版 */

/* 下载完成时调用：登记索引 + 写 .ok */
void cache_add(const Song *s, int bytes);

void cache_remove(long long id);

/* 认领：文件在、索引里没有的歌（写 .ok + 补索引） */
void cache_adopt(long long id, const char *name, const char *artist, int dur_ms, int bytes);
void cache_clear(void);
long long cache_total_bytes(void);

/* 每次缓存变化 +1，UI 用它决定要不要刷新列表 */
int cache_gen(void);

/* 封面缓存 */
int cache_art_read(long long id, unsigned char *buf, int max);
int cache_art_write(long long id, const unsigned char *buf, int len);
void cache_art_drop(long long id);

/* 歌词缓存（cache/<id>.lrc，存接口原始 JSON） */
int cache_lyric_read(long long id, char *buf, int max);
int cache_lyric_write(long long id, const char *buf, int len);
void cache_lyric_remove(long long id);

/* 启动时检查媒体目录，保留完整及部分下载文件。 */
void cache_prune(void);

#endif
