/*
 * MELODY PSP - 纯软件 baseline JPEG 解码器（接口）
 */

#ifndef MELODY_JPEGDEC_H
#define MELODY_JPEGDEC_H

/*
 * 解码到 RGBA8888。
 *   jpg/len  : 输入 JPEG
 *   out      : 输出缓冲，至少要 (min(w,max)*...) 的实际尺寸 * 4 字节
 *   max_dim  : 最长边上限；源图更小时按原尺寸输出（不做放大）
 *   *ow,*oh  : 实际输出尺寸
 * 返回：0 成功；-1 数据错误；-2 内存不够/图太大；-3 渐进式；-4 多扫描
 */
int jpeg_decode_rgba_scaled(const unsigned char *jpg, int len, unsigned char *out,
                            int max_dim, int *ow, int *oh);

/* 只读尺寸；返回 0 成功。*progressive 可选 */
int jpeg_read_size(const unsigned char *jpg, int len, int *w, int *h, int *progressive);

#endif
