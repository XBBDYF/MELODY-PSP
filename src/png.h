/*
 * MELODY PSP - 极简 PNG 解码器
 * ------------------------------------------------------------------
 * 网易有些封面是 PNG（sceJpeg 解不了），这里自带一个最小实现，
 * 输出 RGBA8888，交给 rgba_to_565() 转成屏幕用的 565。
 */

#ifndef MELODY_PNG_H
#define MELODY_PNG_H

/* 成功返回 0，*w_out/*h_out 输出尺寸；失败 -1 */
int png_decode_rgba(const unsigned char *src, int len, unsigned char *out, int out_max,
                    int *w_out, int *h_out);

/* 解码时直接盒式缩小到最长边 max_dim（省内存，大封面用这个） */
int png_decode_rgba_scaled(const unsigned char *src, int len, unsigned char *out, int max_dim,
                           int *w_out, int *h_out);

int png_decode_rgba_workspace(const unsigned char*src,int len,unsigned char*out,int out_max,int*w,int*h,unsigned char*work,int size);
#endif
