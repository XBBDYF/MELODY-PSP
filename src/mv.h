#ifndef MELODY_MV_H
#define MELODY_MV_H
#include "mv_settings_contract.h"
int mv_decoder_mode(void);
int mv_soft_quality(void);
int mv_stream_enabled(void);
int mv_resolution(void);
int mv_download_resolution(void);
int mv_buffer_seconds(void);
int mv_set_resolution(int value);
int mv_set_buffer_seconds(int value);
void mv_stream_duration(int milliseconds);
long long mv_stream_prefetch(void);
int mv_stream_buffering(void);
int mv_set_decoder_mode(int mode);
int mv_set_soft_quality(int quality);
int mv_set_stream_enabled(int enabled);

/* In-app MV playback (CPU-only WiliMini decoder).
 * Call mv_start() from the main thread, then mv_display_tick() each frame. */
int  mv_start(int mvid, const char *path, const char *title);
void mv_stop(void);
int  mv_display_tick(void);
int  mv_active(void);
int  mv_finished(void);
int  mv_join(void); /* 0: UI resources released; <0: live worker retained. */
int  mv_result(void);
int  mv_position_ms(void);   /* 当前播放位置（毫秒），调试/抓帧对齐用 */

/* ---- 边下边播（实验，mvstream.txt=1 才启用） ----
 * 下载线程顺序写 <final>.part，播放器通过 PspVideoSource.read_at 读这个增长文件；
 * 数据没到就阻塞等待。返回 0 = 已启动下载。 */
void mv_stream_abort(void);
int  mv_stream_cancelled(void);
/* Call once before high UI assets/GU allocation. Starts an idle persistent
 * worker with a reserved 64KiB stack; performs no network or file I/O.
 * Idempotent; returns 0 on success, negative SDK error on failure. */
int  mv_stream_prepare_worker(void);
int  mv_stream_start(const char *url, const char *final_path);
long long mv_stream_available(void);
long long mv_stream_total(void);
int  mv_stream_active(void);
int  mv_stream_worker_active(void);
int  mv_stream_done(void);
int  mv_stream_error(void);
long long mv_stream_rate_bps(void);
long long mv_stream_required_bps(void);
int  mv_stream_sustainable(void);
long long mv_stream_eta_ms(void);
const char *mv_stream_part_path(void);
/* 用流式源起播（final_path 是最终文件名，实际读 <final>.part） */
/* Native AVC supports compatible finite progressive MP4 sources. */
int  mv_native_requested(void);
int  mv_start_stream(const char *final_path, const char *title);

/* 下载页同时列出 ms0:/VIDEO 与 ms0:/PSP/VIDEO 的本地视频。 */
#define MV_DL_MAX 32
typedef struct {
    int mvid;
    char name[72];
    char artist[64];
    char path[128];
    int size;
    int state;      /* 1 下载中 2 已完成 3 未完成/可继续 */
    int pct;
} MvDlItem;

int mv_dl_snapshot(MvDlItem *out, int max);
int mv_dl_delete(int idx);
/* Re-scan ms0:/VIDEO on the next list/draw (settings toggle). */
void mv_local_video_rescan(void);

/* MV 列表索引（cache/mv_index.txt）：下载完成、播放本地 MV 时写入；
 * 启动时 mv_index_init() 在索引不存在时扫一次目录做 bootstrap。 */
void mv_index_add(int mvid, int size);
void mv_index_init(void);

#endif
