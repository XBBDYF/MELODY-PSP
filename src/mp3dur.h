#ifndef MELODY_MP3DUR_H
#define MELODY_MP3DUR_H

/* 按 MPEG 帧精确计算本地 mp3 时长（毫秒）。
 * conf 可传 NULL；返回 0 表示读不出来。conf: 2 精确/CBR、1 VBR 估算、0 未知。 */
int mp3_dur_ms(const char *path, int *conf);

/* 从文件名里取网易云歌曲 id（<id>.mp3 / MELODY-<id>.mp3），不是就返回 0。 */
long long mp3_id_from_path(const char *path);

#endif
