/*
 * MELODY PSP — 二维码生成（见 qr.c）
 */

#ifndef MELODY_QR_H
#define MELODY_QR_H

/*
 * 把 text 编码成二维码。
 *   out        : 输出缓冲（至少 57*57 字节），按 dim 紧凑存 1 bit/模块（1 = 黑）
 *   out_stride : 兼容参数，现在忽略
 *   *out_dim   : 实际边长（模块数）
 * 返回边长，失败 <0。
 */
int qr_encode(const char *text, unsigned char *out, int out_stride, int *out_dim);

#endif