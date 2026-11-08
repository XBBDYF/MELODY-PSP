/*
 * MELODY PSP — 日志 + 全局文件锁
 *
 * PSP 的 newlib stdio 不是线程安全的：四个线程同时 fopen/fwrite
 * 会踩坏 FILE 链表（表现为随机假死）。所有 stdio 操作都过这里的一把锁。
 */

#ifndef MELODY_LOG_H
#define MELODY_LOG_H

void log_open(void);
void log_close(void);
void log_flush(void);   /* 把缓冲写盘（主循环每 2 秒调一次） */

/* 全局 stdio 互斥（可重入会死锁，调用者自己保证不嵌套）。
 * 内部最多等 100ms：线程被强杀时锁可能永远回不来，超时就跳过这次写盘，
 * 绝不让主线程永久卡在日志上（见 docs/ppsspp-menu-hang.md）。 */
void io_lock(void);
void io_unlock(void);
void io_lock_reset(void);     /* 强杀线程后换一把新锁（旧锁可能被带走） */
int io_lock_timeouts(void);   /* 超时次数（诊断用） */

/* 启动到现在的毫秒数（性能测量用） */
int log_ms(void);

/* 注意：不能叫 logf —— 会和 math.h 的 logf(float) 冲突 */
void mlog(const char *fmt, ...);

/* 调试用：绕过 stdio 和 io_lock 直接写卡（查死锁时用） */
void log_raw(const char *s);

#endif
