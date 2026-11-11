/*
 * MELODY PSP — 播放控制
 * ------------------------------------------------------------------
 * 三条线程：
 *   audio  ：把 cache/<id>.mp3 喂给 sceMp3 → 重采样 44.1kHz → sceAudio 输出
 *   dl     ：并行分片下载，往同一个文件里写（见 dl.c）
 *   ui     ：主线程，只读状态，不阻塞
 *
 * seek 只动音频线程自己的读指针，不换歌、不重下。
 */

#ifndef MELODY_AUDIO_H
#define MELODY_AUDIO_H

#include "app.h"

enum {
    AUDIO_IDLE = 0,
    AUDIO_BUFFER,     /* 等数据 / 建解码器 */
    AUDIO_PLAY,
    AUDIO_PAUSE,
    AUDIO_END,
    AUDIO_ERROR
};

int audio_init(void);
void audio_shutdown(void);

/* 在线播放（边下边播），返回这次播放的 gen（>0） */
int audio_play_stream(const Song *s, const char *url, int total);

/* 播放已缓存的整首（不联网） */
int audio_play_cached(const Song *s);

/* 播放任意本地文件（本地音乐 / 导出的歌） */
int audio_play_file(const char *path, const Song *meta);

/* Main-thread MV handoff. Negative means codec ownership is unconfirmed:
 * do not start MV or resume MP3. Resume only after native stop/unload. */
int audio_mv_suspend(void);
int audio_mv_resume(void);

void audio_stop(void);
/* Replay the same fully local/downloaded source via fresh decoder generation. */
int audio_replay_current(const Song *meta);
void audio_pause(int on);
int audio_paused(void);

/* 跳转到 ms（只在当前这首歌里跳，不重新下载） */
void audio_seek(int ms);

int audio_state(void);
int audio_needs_restart(void); /* sticky codec handoff fault; no automatic retries */
int audio_pos_ms(void);
int audio_dur_ms(void);
int audio_rate(void);
int audio_kbps(void);

/* 整首缓存进度：0..100 正在下，-1 已完整 / 本地文件 */
int audio_cache_pct(void);

/* 已经下好、可以立刻播的时长（毫秒） */
int audio_buffered_ms(void);

int audio_set_volume(int v);      /* 0..100 */
int audio_volume(void);
int audio_level(void);

const char *audio_error(void);

/* 诊断：输出欠载次数 / 最大间隔（毫秒）。>40ms 的间隔说明音频硬件饿过 */
void audio_stall_stats(int *count, int *max_ms);

/* 调试：dump N 个输出块到 pcm_out.raw / pcm_dec.raw */
void audio_debug_dump(int blocks);

/* 读文件估时长（缓存认领用），失败返回 0 */
int audio_probe_duration(const char *path, int *kbps_out);

/* 抽样校验缓存文件是否完整（1 = 好） */
int audio_file_ok(const char *path);

#endif
