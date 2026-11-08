/*
 * MELODY PSP — MV 边下边播数据源（P0：顺序下载 + 盘上读）
 *
 * 播放器（melody_mv.prx / pspvideo）只通过 PspVideoSource 访问数据：
 *   read_at  随机读（视频和音频线程会并发调用）
 *   cancel   永久取消
 * 这里把「HTTP 顺序下载到 <final>.part」和「按偏移读盘」接起来：
 * 下载线程只把 avail 往前推，读线程在 [0,avail) 里直接读盘，不够就等。
 *
 * P0 边界（P1 再做）：
 *   - 不支持 seek/重启 Range：interrupt_reads 固定为 NULL，size 用 probe 到的
 *     真实值（因此解码器认为源可 seek，向前 seek 超出 avail 会等到超时失败）。
 *   - 网络错误不续传：直接置 err，由调用方回退整包下载（api.c 的 Range 续传）。
 *   - 不写 sidecar（需要标题/时长，留给 mv.c 在 complete 后补）。
 *
 * 线程模型：下载线程 prio 0x28、栈 0x8000；Http 结构约 16KB，必须静态，
 * 绝不能放栈（PSP 线程栈只有几十 KB）。生命周期由调用方掌握：
 *   mvsrc_open() → （水位到了）mv_start_stream() → mv_join() 里 mvsrc_close()
 */

#ifndef MELODY_MVSRC_H
#define MELODY_MVSRC_H

#include "psp_video.h"

#ifdef __cplusplus
extern "C" {
#endif

/* probe + 起下载线程。返回 0 = 可流式；<0 = 失败（调用方回退整包下载）。
 * 同步阻塞到拿到 Content-Length（最长约 15 秒）。 */
int mvsrc_open(const char *url, const char *final_path);

/* 取消并回收线程/fd/HTTP；可重复调用；随后可再次 mvsrc_open。 */
void mvsrc_close(void);

/* 只要取消，不 join（下载页点取消时用；之后仍需 mvsrc_close 收尾）。 */
void mvsrc_abort(void);

/* 下载进行中（未完成、未失败、未取消）。 */
int mvsrc_started(void);

/* 总字节数（probe 得到，>0）；未打开时返回 -1。 */
long long mvsrc_size(void);

/* 已连续落盘的字节数 [0,avail)。 */
long long mvsrc_available(void);

/* 已下满并改名成 final。 */
int mvsrc_complete(void);

/* 发生致命错误（网络/写盘/改名）。 */
int mvsrc_error(void);

/* 填 PspVideoOptions.source 用；未打开返回 NULL。P0 里 interrupt_reads = NULL。 */
const PspVideoSource *mvsrc_source(void);

/* <final>.part 的路径；未打开返回 NULL。 */
const char *mvsrc_part_path(void);

#ifdef __cplusplus
}
#endif

#endif /* MELODY_MVSRC_H */
