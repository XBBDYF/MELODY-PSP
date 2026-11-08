/*
 * MELODY PSP — TLS/HTTPS 客户端（mbedTLS 2.28 LTS 后端）
 * ------------------------------------------------------------------
 * 只做一件事：把一条「已经连好的 TCP socket」包成 TLS 会话，让上层
 * 用最朴素的阻塞 read/write 收发。不碰 socket 本身（不 connect / 不 close），
 * 也不改任何全局网络状态，方便直接塞进现有的 http 客户端里。
 *
 * 典型用法（net.c 里）：
 *
 *     sock = tcp_connect(host, 443, ...);
 *     void *tls = tls_start(sock, host, err, errlen);
 *     if (!tls) { close(sock); return -1; }
 *     tls_write(tls, req, reqlen);
 *     while ((n = tls_read(tls, buf, sizeof buf)) > 0) { ... }
 *     tls_stop(tls);          // 不关 socket
 *     close(sock);
 *
 * 证书校验默认打开（只校验证书链 + 主机名，不校验有效期 —— PSP 的 RTC
 * 掉电后可能是 2000 年）。用 tls_set_verify(0) 可以整个跳过校验。
 */

#ifndef MELODY_TLS_H
#define MELODY_TLS_H

/* 返回 1 表示模块已编译进来并且初始化成功（mbedTLS 种子/根证书都就绪）。 */
int tls_available(void);

/* 初始化（幂等）。第一次调用会：
 *   - 从 PSP 时间/RTC/地址熵 + 持久化 seed 文件里采熵，种 CTR-DRBG
 *   - 解析内置根证书
 * 失败也不会崩，tls_available() 会返回 0。 */
void tls_init(void);

/* 释放全局状态（进程退出前调用，不调用也不会泄漏）。 */
void tls_term(void);

/* 1 = 校验证书链 + 主机名（默认）；0 = 接受任何证书（调试/自签服务器用）。 */
void tls_set_verify(int on);
int  tls_verify_enabled(void);

/* 把一条已连接的 TCP socket 包成 TLS 会话。host 用于 SNI 和证书主机名校验。
 * 成功返回不透明句柄；失败返回 NULL，err/errlen 里是简短的 ASCII 原因。
 * 注意：句柄内部串行化握手（见 README），所以多个线程同时 tls_start 是安全的，
 * 只是握手会排队。 */
void *tls_start(int sock, const char *host, char *err, int errlen);

/* 阻塞读/写。read：>0 字节数，0 = 对端正常关闭，<0 = 出错（连接已不可用）。
 * write：写入的字节数（全部写完才返回），<0 = 出错。
 * 单次调用内部有 30 秒空闲上限，避免网络死了以后线程永远回不来。 */
int  tls_read(void *h, void *buf, int len);
int  tls_write(void *h, const void *buf, int len);

/* 释放句柄。不关闭 socket，调用者自己 close()。 */
void tls_stop(void *h);

/* ------------------------------------------------------------------ */
/* 以下为可选扩展：不集成也能用，测试 EBOOT / 日志用它拿诊断信息。    */
/* ------------------------------------------------------------------ */

int tls_handshake_ms(void *h);      /* tls_start 里握手用了多少毫秒，<0 未知 */
int tls_first_byte_ms(void *h);     /* 握手完成到第一次 tls_read 拿到数据 */
int tls_set_timeout_ms(void *h, int ms);   /* 改单次读写的空闲上限，返回旧值 */
const char *tls_cipher_name(void *h);      /* "ECDHE-RSA-AES256-GCM-SHA384" */
const char *tls_protocol_name(void *h);    /* "TLSv1.2" */
const char *tls_mbedtls_version(void);     /* 编译进来的 mbedTLS 版本 */
void tls_set_log(void (*fn)(const char *fmt, ...));   /* 接 mlog() 即可 */
void tls_set_trace(int on);        /* 1 = 每个握手步骤都打日志（排障用） */
void tls_set_cert_dir(const char *dir);   /* 调试：把对端证书链 dump 到该目录 */

#define MELODY_TLS_H_VERSION 2

#endif /* MELODY_TLS_H */
