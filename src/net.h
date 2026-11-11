/*
 * MELODY PSP — 网络层
 * ------------------------------------------------------------------
 * PSP 只有明文 HTTP（没有可用的 TLS 栈），所以所有请求都是 http://。
 * 自带一个够用的 HTTP/1.0 客户端：
 *   - Content-Length / chunked / 读到断开 三种长度模式都支持
 *   - Range 断点（多连接并行下载、快进都用它）
 *   - 短超时 socket：下载线程可以随时被中断，退出不再卡在 recv 里
 *   - DNS 缓存（PSP 的解析一次要几百毫秒，CDN 主机名还老变）
 *
 * 注意：PSP 是「按需联网」，XMB 里的连接不会自动带到程序里，
 * 所以要先 apctl connect，或者探测一下（PPSSPP 不需要 apctl）。
 */

#ifndef MELODY_NET_H
#define MELODY_NET_H

#define NET_POOL_SIZE   (768 * 1024)   /* 4~6 条下载连接 × 64KB + API + 封面 */
#define NET_APCTL_STACK 0x8000

/* Main-thread final network gate. Offline shuts down all owned sockets;
 * workers observe cancellation and close their own descriptors. */
void net_set_offline(int offline);
int net_offline(void);
/* Nonblocking utility reservation: 1 acquired, 0 wait for workers, <0 denied.
 * begin revokes new HTTP/DNS/APCTL work immediately; end releases reservation. */
int net_dialog_begin(void);
void net_dialog_end(int connected);

int net_init(void);
int net_speed_kbps(void);
int net_peak_speed_kbps(void);
void net_speed_tick(void);
void net_term(void);

/* 全局 Cookie（登录后带 MUSIC_U，所有请求自动附带） */
void net_set_cookie(const char *c);
const char *net_get_cookie(void);
/* 最近一次响应头里抓到的 MUSIC_U / __csrf（多个用 "; " 拼好） */
const char *net_last_set_cookie(void);
/* 临时不发送 Cookie（取播放地址时用匿名身份，免得拿到会话签名 URL 被 CDN 403） */
void net_skip_cookie(int skip);

int net_apctl_state(void);          /* PSP_NET_APCTL_STATE_* */
int net_apctl_connect(int profile, int seconds);
/* Last profile that reached GOT_IP (0 = unknown). Boot auto-connect tries it first. */
int net_last_profile(void);
/* Explicit user reconnect: retire idle sockets/DNS and re-associate, even GOT_IP. */
int net_manual_connect(int profile,int seconds);
int net_apctl_disconnect(void);
int net_utility_dialog(void);       /* 系统「网络连接」对话框 */
int net_current_ip(char *out, int outlen);
int net_current_name(char *out, int outlen);

/* WLAN 开关（PSP-1000 侧面那个拨钮），1 开 / 0 关 */
int net_wlan_switch(void);

/* 已保存的网络连接（sceUtilityCheckNetParam / GetNetParam） */
#define NET_MAX_PROFILES 8

typedef struct {
    int id;
    char name[64];
    char ssid[64];
    int secure;         /* 0 开放 / 1 WEP / 2 WPA，<0 未知 */
} NetProfile;

int net_list_profiles(NetProfile *out, int max);

/* 直接 TCP 探测：通了就不用管 apctl 状态（PPSSPP 就是这种） */
int net_probe_host(const char *host, int port, int timeout_ms);
/* Nonblocking download-worker reconnect kick, throttled with API reconnects. */
int net_reconnect_poll(void);
int net_reconnect(void);          /* WLAN 掉线后自动连回去 */

/* ---------------- HTTP ---------------- */

#define HTTP_BUF_SIZE 16384

typedef struct {
    int sock;
    int chunked;
    int gzip;                       /* 响应带 Content-Encoding: gzip */
    int keep_alive;                 /* 这次请求允许复用连接 */
    int reused;                     /* 这条连接是从池子里拿的 */
    int no_reuse;                   /* 复用的连接坏了：本次重连后不要再取池子 */
    int reused_broken;              /* 这条连接不能放回池子 */
    int chunk_left;
    /* chunk framing must survive a short recv timeout between any two bytes. */
    int chunk_state;                 /* size / data / data CR / data LF / trailer / error */
    int chunk_size;                  /* checked size accumulator, <= INT_MAX */
    int chunk_digits;
    int chunk_ext;
    int chunk_cr;
    int chunk_line_len;              /* bounded size/trailer line, including CRLF */
    int chunk_trailer_bytes;         /* bounded complete trailer section */
    int finished;
    long long remaining;            /* -1 = 未知 */
    int status;
    long long content_length;
    long long range_from;           /* 请求的起始偏移 */
    long long range_start;          /* Validated Content-Range; all three -1 when absent/invalid. */
    long long range_end;
    long long range_total;
    long long body_bytes;           /* 已读正文字节 */
    int body_timeout;               /* 正文 recv 的超时（毫秒） */
    int eof_retry;                  /* 长度没读满却收到 0 的重试次数 */
    long long bytes_total;          /* 统计：所有 recv 到的原始字节 */
    unsigned char buf[HTTP_BUF_SIZE];
    int blen;
    int bpos;
    char host[96];
    int port;
    char path[768];
} Http;

/* 打开连接并读完响应头。返回 0 成功 / <0 失败（err 里是原因）。 */
int http_open(Http *h, const char *url, long long range_from,
              int timeout_ms, char *err, int errlen);
/* 大文件整包下载：不进 keep-alive 池，接收窗 64KB（MV 用） */
int http_open_bulk(Http *h, const char *url, int timeout_ms, char *err, int errlen);

/* 带结束偏移的 Range 请求（to < 0 表示不指定结尾） */
int http_open_range(Http *h, const char *url, long long from, long long to,
                    int timeout_ms, char *err, int errlen);

/* Generation-aware open. Cancels when *cancel != cancel_value, including
 * connection-lock wait, TCP handshake, request send and HTTP-header wait.
 * Resolver calls observe cancellation on entry/exit (SDK DNS: 2s, 2 retries).
 * This scope belongs to the sole download worker, never to API abort state. */
int http_open_range_cancel(Http *h, const char *url, long long from, long long to,
                           int timeout_ms, volatile int *cancel, int cancel_value,
                           char *err, int errlen);

/* 设置正文读取时 recv 的超时（毫秒）。下载线程用 2ms 就能随时被中断。 */
int http_set_timeout(Http *h, int ms);

/*
 * 读正文。>0 读到的字节；0 结束；-1 暂时没数据（可重试）；
 * -2 出错；-3 被中断（*gen 变了）。
 * idle_ms <= 0：只做一次非阻塞尝试（socket 超时由 http_set_timeout 决定）。
 * idle_ms  > 0：最多等这么久，其间反复重试。
 */
int http_read(Http *h, char *dst, int len, int idle_ms,
              volatile int *gen, int my_gen, char *err, int errlen);

void http_close(Http *h);

/* Register the calling thread's small-request abort flag; other workers ignore it. */
void http_set_abort(volatile int *flag);

/* 一次性把（较小的）响应读进内存 */
int http_post_form(const char *url,const char *body,char *out,int outmax,int *outlen,int timeout_ms,char *err,int errlen);
int http_get(const char *url, char *out, int outmax, int *outlen,
             int timeout_ms, char *err, int errlen);

/* 上一次 http_get 的 Content-Length（-1 = 未知），用来判断是否被截断 */
int http_get_expect(void);

/* 把 UTF-8 字符串做 URL 编码 */
void net_url_encode(const char *src, char *dst, int dstlen);

/* 关掉所有空闲的 keep-alive 连接（api 线程重启前用，避免复用半死的 socket） */
void net_drop_idle(void);

/* api 线程被强杀后重建 net 全局锁（避免锁被带走导致网络永久阻塞） */
void net_reset_locks(void);

#endif
