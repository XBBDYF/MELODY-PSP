/*
 * MELODY PSP — 下载引擎（多连接 + 分片 + 断点）
 * ------------------------------------------------------------------
 * 为什么是分片 + 多连接：
 *   PSP 的 TCP 接收窗口很小，单条连接的吞吐被「窗口 / RTT」锁死
 *   （实测 ~100-150KB/s，而 802.11b 链路本身能跑 300KB/s+）。
 *   把文件切成 128KB 的分片，用 3 条连接按顺序抢分片，
 *   总吞吐基本能翻倍，而且分片是从头往后取的，
 *   所以「已经连续下载好的前缀」增长很快 —— 播放在前缀里就能放。
 *
 * 线程只有一个（dl 线程），所有 socket 在一个循环里轮询，
 * 每个 socket 的 recv 超时是 2ms，所以随时可以被打断。
 */

#ifndef MELODY_DL_H
#define MELODY_DL_H

#include "app.h"

#define DL_QUEUE_MAX 32

enum {
    DL_PENDING = 0,
    DL_ACTIVE,
    DL_DONE,
    DL_FAILED,
    DL_PAUSED
};

typedef struct {
    long long id;
    Song song;
    int total;
    int state;
    int pct;
} DlItem;

void dl_init(void);
void dl_set_video_priority(int enabled);
/* 返回 1 = 线程没在超时内退出、被强杀了（此时别再去碰文件系统的全局锁） */
int  dl_shutdown(void);

/* 并发连接数（2..6，默认 4）。PSP-1000 的 802.11b 上 4 路最稳，
 * 信号好时可以试 5/6 路；设置页里能改。 */
void dl_set_parts(int n);
int  dl_get_parts(void);

/* ---- 播放用的下载流（同时只有一条） ---- */
int  dl_stream_start(const Song *s, const char *url, int total, int gen);
void dl_stream_stop(void);
int  dl_stream_active(void);
/* Bytes in the contiguous, actually written prefix; excludes sparse holes. */
long long dl_stream_available(int gen);
/* Clamp a decoder read to that prefix and publish demand for low-water priority.
 * Returns 0 for stale/pending generations or an unavailable offset. */
int  dl_stream_readable(int gen, long long off, int want);
int  dl_stream_total(int gen);
int  dl_stream_done(int gen);
int  dl_stream_failed(int gen);
const char *dl_stream_error(void);
long long dl_stream_bytes(void);

/* ---- 下载队列 ---- */
/* Includes cached songs, an accepted stream command and queued/active jobs. */
int  dl_has_song(long long id);
int  dl_enqueue(const Song *s, const char *url, int total);
int  dl_queue_count(void);
const DlItem *dl_queue_at(int i);
int  dl_queue_snapshot(DlItem *out, int max);
/* 0 = accepted, -1 = invalid index/state. Active pause is worker-owned;
 * observe DL_PAUSED via snapshot/gen after buffers and checkpoint are closed.
 * Restored queues start paused; resume revalidates the existing cache prefix. */
int  dl_queue_pause(int index);
int  dl_queue_resume(int index);
void dl_queue_cancel(int index);
void dl_queue_clear(void);
int  dl_queue_gen(void);
int  dl_speed_kbps(void);

#endif
