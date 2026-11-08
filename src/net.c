/*
 * MELODY PSP — 网络层实现
 *
 * 重点（都是为了「快」和「能中断」）：
 *   1. SO_RCVBUF 调大 —— PSP 默认的接收窗口很小，窗口/RTT 直接决定吞吐上限。
 *   2. 正文 recv 用短超时（下载线程 2ms），线程随时能看见退出标志，
 *      再也不会卡在 recv 里几十秒。
 *   3. 16KB 缓冲 + 一次 recv 尽量多拿，减少 syscall 次数。
 *   4. DNS 缓存：PSP 解析一次主机名要几百毫秒，CDN 域名每首歌都可能变。
 */

#include "net.h"
#include "app.h"
#include "log.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netconf.h>
#include <psputility_netmodules.h>
#include <psputility_netparam.h>
#include <psprtc.h>
#include <pspwlan.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

#define MAX_REDIRECTS 4
#define HDR_MAX       4608

static void dns_load(void);   /* 启动时从 dns.txt 读回上次解析结果 */
static int dns_wait_ready(int deadline_ms);   /* 等 AP 真正报出 DNS 服务器 */

/* 只有 http_get 需要 gzip：歌词/歌单/评论这些 JSON 压缩后只有 1/6 ~ 1/10，
 * 在 PSP 的 WiFi 上省下来的就是真金白银。音频/图片走同一套代码但不解压。 */
static int g_want_gzip;

/* ---------------- 连接复用（keep-alive） ----------------
 * PSP 上每次 TCP 握手 + DNS 要几百毫秒，比 7KB 的 JSON 本身还贵。
 * 歌单/评论/封面都连到同一两个域名，复用几条空闲连接就能把
 * 「每张封面一次握手」变成零。 */
#define KA_MAX 4
typedef struct {
    int sock;
    char host[96];
    int port;
    unsigned last;
} KeepAlive;

static KeepAlive g_ka[KA_MAX];
static int g_ka_ready;
static int g_want_keepalive;

static void ka_init(void)
{
    if (g_ka_ready) return;
    g_ka_ready = 1;
    for (int i = 0; i < KA_MAX; ++i) g_ka[i].sock = -1;
}

static void ka_drop(int i)
{
    if (g_ka[i].sock >= 0) close(g_ka[i].sock);
    g_ka[i].sock = -1;
    g_ka[i].host[0] = 0;
}

/* 取一条可复用的连接（已经从池子里摘掉，调用方负责关闭或放回） */
static int ka_take(const char *host, int port)
{
    ka_init();
    for (int i = 0; i < KA_MAX; ++i) {
        if (g_ka[i].sock >= 0 && g_ka[i].port == port && !strcmp(g_ka[i].host, host)) {
            int s = g_ka[i].sock;

            g_ka[i].sock = -1;
            g_ka[i].host[0] = 0;
            mlog("ka reuse %s:%d", host, port);
            return s;
        }
    }
    return -1;
}

static void ka_put(const char *host, int port, int sock)
{
    int slot = -1;

    ka_init();
    for (int i = 0; i < KA_MAX; ++i) {
        if (g_ka[i].sock < 0) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < KA_MAX; ++i)
            if (g_ka[i].last < g_ka[slot].last) slot = i;
        ka_drop(slot);
    }
    g_ka[slot].sock = sock;
    snprintf(g_ka[slot].host, sizeof(g_ka[slot].host), "%s", host);
    g_ka[slot].port = port;
    g_ka[slot].last = (unsigned)sceKernelGetSystemTimeLow();
}

static void ka_close_all(void)
{
    for (int i = 0; i < KA_MAX; ++i) ka_drop(i);
}

/* 全局打断标志：api 线程要跑播放请求时，把正在读的小请求打断 */
static volatile int *g_abort_flag;
static SceUID g_abort_owner = -1;
/* Only the range-opening thread observes this temporary generation context.
 * Other threads retain their own API abort semantics. */
static SceUID g_cancel_owner = -1;
static volatile int *g_cancel_flag;
static int g_cancel_value;

/* 全局 Cookie（登录后带上 MUSIC_U），以及最近一次响应的 Set-Cookie */
static char g_cookie[1280];
static char g_set_cookie[1280];
static char g_cookie_hdr[1400];

void net_set_cookie(const char *c)
{
    size_t n;

    snprintf(g_cookie, sizeof(g_cookie), "%s", c ? c : "");
    /* 网易的取流接口要求带 os=pc + appver 才肯回地址（否则 code -110）。
     * 这两项和登录态无关，匿名也要带；MUSIC_U 仍然只发给 API 域名。 */
    n = strlen(g_cookie);
    if (!strstr(g_cookie, "os=pc") && n + 32 < sizeof(g_cookie))
        snprintf(g_cookie + n, sizeof(g_cookie) - n, "%sos=pc; appver=2.9.7", n ? "; " : "");
}

const char *net_get_cookie(void) { return g_cookie; }

const char *net_last_set_cookie(void) { return g_set_cookie; }

/* 从响应头里抓一对 cookie（MUSIC_U / __csrf），多个用 "; " 拼起来 */
static void grab_cookie_pair(const char *buf, const char *name)
{
    const char *p = strstr(buf, name);
    char tmp[300];
    int n = 0, cur;

    if (!p) return;
    while (p[n] && p[n] != ';' && p[n] != '\r' && p[n] != '\n' && n < (int)sizeof(tmp) - 1) {
        tmp[n] = p[n];
        n++;
    }
    tmp[n] = 0;
    cur = (int)strlen(g_set_cookie);
    if (cur + n + 3 < (int)sizeof(g_set_cookie))
        snprintf(g_set_cookie + cur, sizeof(g_set_cookie) - cur, "%s%s", cur ? "; " : "", tmp);
}

static int g_skip_cookie;

void net_skip_cookie(int skip) { g_skip_cookie = skip; }

static const char *cookie_hdr(const char *host)
{
    if (!g_cookie[0]) return "";
    /* 关键：MUSIC_U 只能发给音乐 API 的域名。发给 CDN（*.music.126.net）
     * 会让它拿签名 URL 和会话令牌比对，直接 403 —— 以前所有免费歌都是
     * 因为这一条被误判成「没版权」。 */
    if (!host || !strstr(host, "music.163.com")) return "";
    if (g_skip_cookie) {
        /* 匿名取流：MUSIC_U 不带（否则拿到会话签名 URL 会被 CDN 403），
         * 但 os=pc / appver 必须带，否则播放地址接口回 code -110。 */
        snprintf(g_cookie_hdr, sizeof(g_cookie_hdr), "Cookie: os=pc; appver=2.9.7\r\n");
        return g_cookie_hdr;
    }
    snprintf(g_cookie_hdr, sizeof(g_cookie_hdr), "Cookie: %s\r\n", g_cookie);
    return g_cookie_hdr;
}

void http_set_abort(volatile int *flag)
{
    g_abort_flag = flag;
    g_abort_owner = sceKernelGetThreadId();
}

static int aborted(void)
{
    SceUID self = sceKernelGetThreadId();
    if (self == g_cancel_owner && g_cancel_flag)
        return *g_cancel_flag != g_cancel_value;
    return self == g_abort_owner && g_abort_flag && *g_abort_flag;
}


/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static char lower_ch(char c)
{
    return (char)tolower((unsigned char)c);
}

/* 大小写无关的 strstr */
static char *ci_strstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);

    if (nl == 0) return (char *)hay;
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] && lower_ch(p[i]) == lower_ch(needle[i])) i++;
        if (i == nl) return (char *)p;
    }
    return NULL;
}

/* Called under conn_lock; preserve the header for subsequent field lookups. */
static char *header_value(char *hdr, const char *name)
{
    static char value[1024];
    size_t nl = strlen(name);
    char *line = hdr;

    while (line && *line) {
        char *eol = strstr(line, "\r\n");
        if (!eol) eol = line + strlen(line);

        if ((size_t)(eol - line) > nl && !strncasecmp(line, name, nl) && line[nl] == ':') {
            char *v = line + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t length=(size_t)(eol-v);
            if(length>=sizeof(value))return NULL;
            memcpy(value,v,length);value[length]=0;
            return value;
        }
        if (!*eol) break;
        line = eol + 2;
    }
    return NULL;
}

/* Publish a satisfied byte range atomically only after full syntax/bounds checks. */
static void parse_content_range(Http *h, const char *value)
{
    long long fields[3];
    int i;
    const char *p = value;
    h->range_start = h->range_end = h->range_total = -1;
    if (!p || strncasecmp(p, "bytes ", 6)) return;
    p += 6;
    for (i = 0; i < 3; i++) {
        long long number = 0;
        if (*p < '0' || *p > '9') return;
        do {
            int digit = *p++ - '0';
            if (number > (9223372036854775807LL - digit) / 10) return;
            number = number * 10 + digit;
        } while (*p >= '0' && *p <= '9');
        fields[i] = number;
        if (i < 2 && *p++ != (i == 0 ? '-' : '/')) return;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p || fields[0] > fields[1] || fields[1] >= fields[2]) return;
    h->range_start = fields[0];
    h->range_end = fields[1];
    h->range_total = fields[2];
}

/* 0 成功；-1 格式错；-2 是 https */
static int parse_url(const char *url, char *host, int hostlen, int *port,
                     char *path, int pathlen)
{
    const char *p, *slash, *hostend, *colon = NULL;

    if (!url) return -1;
    if (!strncasecmp(url, "https://", 8)) return -2;
    if (strncasecmp(url, "http://", 7) != 0) return -1;

    p = url + 7;
    slash = strchr(p, '/');
    hostend = slash ? slash : p + strlen(p);

    for (const char *q = p; q < hostend; ++q) {
        if (*q == ':') { colon = q; break; }
    }

    {
        int hlen = (int)((colon ? colon : hostend) - p);
        if (hlen <= 0 || hlen >= hostlen) return -1;
        memcpy(host, p, (size_t)hlen);
        host[hlen] = 0;
    }

    *port = 80;
    if (colon) {
        int prt = atoi(colon + 1);
        if (prt <= 0 || prt > 65535) return -1;
        *port = prt;
    }

    {
        const char *pp = slash ? slash : "/";
        int plen = (int)strlen(pp);
        if (plen >= pathlen) return -1;
        memcpy(path, pp, (size_t)plen + 1);
    }
    return 0;
}

/* 把 Location（可能是相对路径）拼成绝对 URL */
static void abs_url(const char *base, const char *loc, char *out, int outlen)
{
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) {
        snprintf(out, (size_t)outlen, "%s", loc);
        return;
    }

    {
        char host[96], path[768];
        int port = 80;

        if (parse_url(base, host, sizeof(host), &port, path, sizeof(path)) < 0) {
            snprintf(out, (size_t)outlen, "%s", loc);
            return;
        }

        if (loc[0] == '/') {
            if (port == 80) snprintf(out, (size_t)outlen, "http://%s%s", host, loc);
            else snprintf(out, (size_t)outlen, "http://%s:%d%s", host, port, loc);
        } else {
            char *last = strrchr(path, '/');
            if (last) last[1] = 0;
            else snprintf(path, sizeof(path), "/");
            if (port == 80) snprintf(out, (size_t)outlen, "http://%s%s%s", host, path, loc);
            else snprintf(out, (size_t)outlen, "http://%s:%d%s%s", host, port, path, loc);
        }
    }
}

void net_url_encode(const char *src, char *dst, int dstlen)
{
    static const char *hex = "0123456789ABCDEF";
    int o = 0;

    for (const unsigned char *p = (const unsigned char *)src; *p && o < dstlen - 4; ++p) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 15];
        }
    }
    dst[o] = 0;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

/* 只有整套初始化成功才允许调 apctl/wlan/socket。
 * 以前 net_init 中途失败（内存不够时很常见）后面每个网络调用都会打到没加载的模块上，
 * 真机表现就是启动后立刻闪退；PPSSPP 不会。 */
static int g_net_ok;

int net_init(void)
{
    int r;

    g_net_ok = 0;
    sceUtilityLoadModule(PSP_MODULE_NET_COMMON);
    sceUtilityLoadModule(PSP_MODULE_NET_INET);

    r = sceNetInit(NET_POOL_SIZE, 0x20, 0x1000, 0x20, 0x1000);
    if (r < 0) { mlog("sceNetInit = 0x%08X", r); return r; }

    r = sceNetInetInit();
    if (r < 0) { mlog("sceNetInetInit = 0x%08X", r); return r; }

    r = sceNetResolverInit();
    if (r < 0) { mlog("sceNetResolverInit = 0x%08X", r); return r; }

    r = sceNetApctlInit(NET_APCTL_STACK, 0x30);
    if (r < 0) { mlog("sceNetApctlInit = 0x%08X", r); return r; }

    dns_load();
    g_net_ok = 1;
    mlog("net ready (pool %dKB)", NET_POOL_SIZE / 1024);
    return 0;
}

void net_term(void)
{
    if (!g_net_ok) return;
    g_net_ok = 0;
    ka_close_all();
    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
}

/* 把空闲的 keep-alive 连接全关掉。api 线程被强杀时可能留下读到一半的
 * socket，复用它会一直失败（真机上表现为「MV 回来网络就不行了」）。 */
void net_drop_idle(void)
{
    if (g_net_ok) ka_close_all();
}

int net_apctl_state(void)
{
    int state = -1;

    if (!g_net_ok) return -1;
    if (sceNetApctlGetState(&state) < 0) return -1;
    return state;
}

static unsigned g_auto_association_us;
static int g_auto_association_owned;
static int g_last_profile;     /* 上次连上的配置号（自动重连用） */

int net_apctl_connect(int profile, int seconds)
{
    int state = 0;
    int loops;
    int r;

    if (!g_net_ok) return -1;
    if (profile <= 0) return -1;
    g_auto_association_owned=0; /* Explicit/API connect owns its own deadline. */

    /* 已经拿到 IP 时不能再调 sceNetApctlConnect —— 内核会直接返回错误，
     * 用户看到的就是"在 WiFi 页按 ○ 重连，结果报连接失败"。
     * 对调用方来说这本来就是成功。 */
    if (sceNetApctlGetState(&state) == 0 && state == PSP_NET_APCTL_STATE_GOT_IP) {
        if (g_last_profile <= 0) g_last_profile = profile;
        return 0;
    }

    /* 卡在 SCANNING/JOINING/GETTING_IP 时直接 Connect 会一直失败：
     * 先断开，等状态回到 DISCONNECTED 再连。PSP 的 WLAN 掉线后常见。 */
    if (state != PSP_NET_APCTL_STATE_DISCONNECTED) {
        mlog("apctl disconnect before connect (state=%d)", state);
        sceNetApctlDisconnect();
        for (int i = 0; i < 20; ++i) {
            if (sceNetApctlGetState(&state) == 0 &&
                state == PSP_NET_APCTL_STATE_DISCONNECTED)
                break;
            sceKernelDelayThread(100 * 1000);
        }
    }

    r = sceNetApctlConnect(profile);
    if (r < 0) {
        /* 掉线后立刻 Connect 偶尔会被协议栈拒绝，隔一下再试一次 */
        mlog("apctl connect(%d) = 0x%08X state=%d, retry", profile, r, net_apctl_state());
        sceKernelDelayThread(500 * 1000);
        r = sceNetApctlConnect(profile);
        if (r < 0) {
            mlog("apctl connect(%d) retry = 0x%08X state=%d", profile, r, net_apctl_state());
            return -1;
        }
    }

    loops = seconds * 4;
    for (int i = 0; i < loops; ++i) {
        sceNetApctlGetState(&state);
        if (state == PSP_NET_APCTL_STATE_GOT_IP) { g_last_profile = profile; break; }
        if (state == PSP_NET_APCTL_STATE_DISCONNECTED && i > 8) return -2;
        sceKernelDelayThread(250 * 1000);
    }
    if (state != PSP_NET_APCTL_STATE_GOT_IP) return -3;
    /* GOT_IP precedes a usable DHCP DNS server by seconds on PSP. Waiting here
     * keeps the first lookup from paying that cost inside the resolver (11-46s). */
    (void)dns_wait_ready(6000);
    return 0;
}

int net_apctl_state(void);

int net_last_profile(void) { return g_last_profile; }

/* 网络掉了就自己连回去（PSP 的 WLAN 会在空闲后掉线，掉线后所有域名解析都会
 * 在 1-2ms 内失败，看起来像「DNS 坏了」，其实是没有路由）
 *
 * 注意节流：每个失败的 API 请求都会调到这里，不节流就会变成
 * 每秒好几次 sceNetApctlConnect，反而把 WLAN 状态机搅乱（日志里
 * "net reconnect ... failed" 连续刷屏就是这么来的）。10 秒一次，
 * 且正在连的时候绝不打断。 */
static unsigned g_last_reconn_us;
static int g_reconnecting;

/* Download-worker variant: apctl owns the asynchronous association state
 * machine. Never wait here or disconnect an in-progress API/user association. */
int net_reconnect_poll(void)
{
    unsigned now = sceKernelGetSystemTimeLow();
    int state = net_apctl_state();
    if (state == PSP_NET_APCTL_STATE_GOT_IP){g_auto_association_owned=0;return 0;}
    if(g_auto_association_owned&&state!=PSP_NET_APCTL_STATE_DISCONNECTED){
        if(now-g_auto_association_us<15000000u)return -1;
        g_auto_association_owned=0;g_last_reconn_us=now?now:1;sceNetApctlDisconnect();
        mlog("net reconnect: automatic association deadline; reset once state=%d",state);return -1;
    }
    if(state==PSP_NET_APCTL_STATE_DISCONNECTED)g_auto_association_owned=0;
    if (!g_net_ok || !net_wlan_switch() || g_last_profile <= 0 || g_reconnecting ||
        state != PSP_NET_APCTL_STATE_DISCONNECTED) return -1;
    if (g_last_reconn_us && now - g_last_reconn_us < 10000000u) return -1;
    g_last_reconn_us = now ? now : 1;
    g_reconnecting = 1;
    int result = sceNetApctlConnect(g_last_profile);
    if(result>=0){g_auto_association_us=now;g_auto_association_owned=1;}
    g_reconnecting = 0;
    mlog("net reconnect kick profile=%d result=%d", g_last_profile, result);
    return -1; /* pending or failed; a future poll observes GOT_IP */
}

int net_reconnect(void)
{
    unsigned now = (unsigned)sceKernelGetSystemTimeLow();
    int st = net_apctl_state();

    if (st == PSP_NET_APCTL_STATE_GOT_IP){g_auto_association_owned=0;return 0;}
    if(g_auto_association_owned)return net_reconnect_poll();
    if (st == PSP_NET_APCTL_STATE_SCANNING || st == PSP_NET_APCTL_STATE_JOINING ||
        st == PSP_NET_APCTL_STATE_GETTING_IP) {
        return -1;                  /* 正在连，等它自己走完 */
    }
    if (g_reconnecting) return -1;
    if (g_last_reconn_us && now - g_last_reconn_us < 10u * 1000 * 1000) return -1;
    g_last_reconn_us = now;

    if (g_last_profile <= 0) {
        mlog("net reconnect: no profile");
        return -1;
    }

    mlog("net reconnect (profile %d, state %d)", g_last_profile, st);
    g_reconnecting = 1;
    if (net_apctl_connect(g_last_profile, 12) == 0) {
        g_reconnecting = 0;
        mlog("net reconnect ok");
        return 0;
    }
    g_reconnecting = 0;
    mlog("net reconnect failed");
    return -1;
}

int net_current_ip(char *out, int outlen)
{
    union SceNetApctlInfo info;

    if (outlen > 0) out[0] = 0;
    if (!g_net_ok) return -1;
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) < 0) return -1;
    snprintf(out, (size_t)outlen, "%s", info.ip);
    return 0;
}

int net_current_name(char *out, int outlen)
{
    union SceNetApctlInfo info;

    if (outlen > 0) out[0] = 0;
    if (!g_net_ok) return -1;
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROFILE_NAME, &info) < 0) return -1;
    snprintf(out, (size_t)outlen, "%s", info.name);
    return 0;
}

int net_wlan_switch(void)
{
    int s;

    if (!g_net_ok) return -1;
    s = sceWlanGetSwitchState();
    if (s < 0) return -1;
    return s ? 1 : 0;
}

int net_apctl_disconnect(void)
{
    /* This public entry point is an explicit disconnect, unlike a dropped
     * association. Revoke automatic recovery before the SDK changes state;
     * a later successful explicit connect restores the remembered profile. */
    g_last_profile = 0;g_auto_association_owned=0;
    g_last_reconn_us = 0;
    if (!g_net_ok) return -1;
    return sceNetApctlDisconnect();
}

/* 枚举 XMB 里保存好的连接：先 CheckNetParam 看有没有，再取名字/SSID/加密方式 */
int net_list_profiles(NetProfile *out, int max)
{
    int n = 0;

    if (!g_net_ok) return 0;

    for (int id = 1; id <= NET_MAX_PROFILES && n < max; ++id) {
        netData d;
        int r = sceUtilityCheckNetParam(id);

        if (r != 0) continue;

        out[n].id = id;
        out[n].name[0] = 0;
        out[n].ssid[0] = 0;
        out[n].secure = -1;

        if (sceUtilityGetNetParam(id, PSP_NETPARAM_NAME, &d) == 0)
            snprintf(out[n].name, sizeof(out[n].name), "%s", d.asString);
        if (sceUtilityGetNetParam(id, PSP_NETPARAM_SSID, &d) == 0)
            snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", d.asString);
        if (sceUtilityGetNetParam(id, PSP_NETPARAM_SECURE, &d) == 0)
            out[n].secure = (int)d.asUint;

        mlog("net profile %d: %s / %s / sec %d", id, out[n].name, out[n].ssid, out[n].secure);
        n++;
    }

    mlog("net profiles found: %d, wlan switch %d", n, net_wlan_switch());
    return n;
}

int net_utility_dialog(void)
{
    pspUtilityNetconfData data;
    int done = 0;

    if (!g_net_ok) return -1;
    memset(&data, 0, sizeof(data));
    data.base.size = sizeof(data);
    data.action = PSP_NETCONF_ACTION_CONNECTAP;

    if (sceUtilityNetconfInitStart(&data) < 0) return -1;

    while (!done) {
        int status;

        sceUtilityNetconfUpdate(1);
        status = sceUtilityNetconfGetStatus();

        if (status < 0) break;
        if (status == PSP_UTILITY_DIALOG_NONE) done = 1;
        else if (status == PSP_UTILITY_DIALOG_QUIT) sceUtilityNetconfShutdownStart();
        else sceDisplayWaitVblankStart();
    }

    return (net_apctl_state() == PSP_NET_APCTL_STATE_GOT_IP) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* DNS 缓存                                                            */
/* ------------------------------------------------------------------ */

#define DNS_CACHE_N 40
#define DNS_TTL_SEC (12 * 60 * 60)
#define DNS_FILE    APP_DIR "dns.txt"

typedef struct {
    char host[96];
    unsigned int ip;        /* a.b.c.d 打包成一个 u32，方便存文件 */
    unsigned int ts;        /* unix 秒（RTC） */
} DnsEnt;

static DnsEnt g_dns[DNS_CACHE_N];
static int g_dns_n;

/* PSP 的 RTC tick 从公元 1 年开始算，这里转成 unix 秒（缓存文件才好读） */
static unsigned int dns_now(void)
{
    u64 tick = 0;
    u64 sec;

    if (sceRtcGetCurrentTick(&tick) < 0) return 0;
    sec = tick / 1000000ULL;
    if (sec < 62135596800ULL) return 0;
    sec -= 62135596800ULL;
    if (sec < 1600000000ULL) return 0;     /* RTC 没设过时间就当不可信 */
    return (unsigned int)sec;
}

static unsigned int ip_pack(const unsigned char *b)
{
    return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) |
           ((unsigned int)b[2] << 8) | (unsigned int)b[3];
}

static void ip_unpack(unsigned int v, unsigned char out[4])
{
    out[0] = (unsigned char)(v >> 24);
    out[1] = (unsigned char)(v >> 16);
    out[2] = (unsigned char)(v >> 8);
    out[3] = (unsigned char)v;
}

static void dns_save(void)
{
    FILE *fp;
    unsigned int now = dns_now();

    io_lock();
    fp = fopen(DNS_FILE, "w");
    if (fp) {
        for (int i = 0; i < g_dns_n; ++i)
            fprintf(fp, "%s %u %u\n", g_dns[i].host, g_dns[i].ip, g_dns[i].ts);
        fclose(fp);
    }
    io_unlock();
    (void)now;
}

static void dns_load(void)
{
    FILE *fp;
    char line[160];

    g_dns_n = 0;

    io_lock();
    fp = fopen(DNS_FILE, "r");
    io_unlock();
    if (!fp) return;

    while (fgets(line, sizeof(line), fp) && g_dns_n < DNS_CACHE_N) {
        char host[96];
        unsigned int ip = 0, ts = 0;

        if (sscanf(line, "%95s %u %u", host, &ip, &ts) == 3 && ip) {
            snprintf(g_dns[g_dns_n].host, sizeof(g_dns[g_dns_n].host), "%s", host);
            g_dns[g_dns_n].ip = ip;
            g_dns[g_dns_n].ts = ts;
            g_dns_n++;
        }
    }
    fclose(fp);
    mlog("dns cache: %d hosts, now=%u", g_dns_n, dns_now());
}

static void dns_store(const char *host, unsigned int ip)
{
    int slot = -1;
    unsigned int now = dns_now();

    for (int i = 0; i < g_dns_n; ++i) {
        if (!strcmp(g_dns[i].host, host)) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_dns_n < DNS_CACHE_N) {
            slot = g_dns_n++;
        } else {
            /* 满了：淘汰最久没更新的那条，但永远不动 API 主机 */
            slot = 1;
            for (int i = 1; i < g_dns_n; ++i) {
                if (g_dns[i].ts < g_dns[slot].ts) slot = i;
            }
        }
    }
    snprintf(g_dns[slot].host, sizeof(g_dns[slot].host), "%s", host);
    g_dns[slot].ip = ip;
    g_dns[slot].ts = now;
    dns_save();
}

/* 内置兜底：DNS 彻底失败时用（只用于网易的固定域名，Host 头仍按原域名发） */
static const struct {
    const char *host;
    unsigned int ip;
} g_dns_fallback[] = {
    { "music.163.com",       (112u << 24) | (29u << 16) | (230u << 8) | 13u },
    { "p1.music.126.net",    (112u << 24) | (15u << 16) | (33u << 8) | 41u },
    { "p2.music.126.net",    (223u << 24) | (111u << 16) | (127u << 8) | 41u },
    { "p3.music.126.net",    (223u << 24) | (113u << 16) | (54u << 8) | 239u },
    { "p4.music.126.net",    (36u << 24) | (156u << 16) | (95u << 8) | 158u },
};

static int dns_fallback_ip(const char *host, unsigned int *ip)
{
    for (int i = 0; i < (int)(sizeof(g_dns_fallback) / sizeof(g_dns_fallback[0])); ++i) {
        if (!strcmp(g_dns_fallback[i].host, host)) {
            *ip = g_dns_fallback[i].ip;
            return 1;
        }
    }
    return 0;
}

/* PSP 报告 GOT_IP 早于 DHCP 下发的 DNS 服务器可用。此时调用 SDK 解析器会一直
 * 阻塞等配置：现场日志里第一次解析花了 11.2 秒（dns done ms=11216），另一次会话
 * 46 秒都没返回（log 停在 "dns lookup" 而没有 "dns done"）。
 * 先显式等到 AP 报出 DNS 服务器再解析；等不到就用内置兜底地址，绝不把界面挂在这里。 */
static int apctl_dns_ready(void)
{
    union SceNetApctlInfo info;

    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PRIMDNS, &info) < 0) return 0;
    if (info.primaryDns[0]) return 1;
    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SECDNS, &info) < 0) return 0;
    return info.secondaryDns[0] != 0;
}

/* 0 = ready, -1 = timeout, -2 = cancelled */
static int dns_wait_ready(int deadline_ms)
{
    unsigned t0 = sceKernelGetSystemTimeLow();

    if (apctl_dns_ready()) return 0;
    for (;;) {
        int waited = (int)((sceKernelGetSystemTimeLow() - t0) / 1000);

        if (waited >= deadline_ms) break;
        if (aborted()) return -2;
        sceKernelDelayThread(50 * 1000);
        if (apctl_dns_ready()) {
            mlog("dns ready after %dms", (int)((sceKernelGetSystemTimeLow() - t0) / 1000));
            return 0;
        }
    }
    mlog("dns wait timeout %dms (no DNS server advertised, state=%d)", deadline_ms,
         net_apctl_state());
    return -1;
}

/* ---- 有界解析：SDK 解析器会无限期阻塞，必须放到单独线程 ------------
 *
 * 现场日志里第一次解析 11.2 秒；另一次会话 46 秒没有返回（log 停在 "dns lookup"，
 * 没有 "dns done"）。sceNetResolverStartNtoA 是同步调用，timeout/retry 参数约束
 * 不了它，调用方也没有任何办法中断。所以把它挪到常驻 worker：
 *   - 提交任务后最多等 2500ms，超时就当失败，界面/播放绝不再陪它等；
 *   - worker 卡死时会被"放弃"（最多创建 3 个），后续任务继续尝试；
 *   - 结果用代次号发布，陈旧结果不会冒充新任务的答案。 */
#define DNS_JOB_DEADLINE_MS 2500
#define DNS_WORKER_MAX      3
#define DNS_WORKER_STACK    0x4000

static volatile int g_dns_state;      /* 0 idle / 1 running / 2 ok / 3 fail */
static volatile int g_dns_gen, g_dns_done_gen;
static volatile unsigned int g_dns_ip;
static char g_dns_host[128];
static SceUID g_dns_wake = -1;
static int g_dns_workers;

static int dns_worker_step(void)
{
    unsigned char buffer[1024];
    struct in_addr address;
    char host[128];
    int rid, result, my;

    my = g_dns_gen;
    snprintf(host, sizeof(host), "%s", g_dns_host);
    result = sceNetResolverCreate(&rid, buffer, sizeof(buffer));
    if (result >= 0) {
        result = sceNetResolverStartNtoA(rid, host, &address, 2, 2);
        sceNetResolverDelete(rid);
    }
    if (result >= 0) {
        unsigned irq = sceKernelCpuSuspendIntr();
        g_dns_ip = ip_pack((const unsigned char *)&address.s_addr);
        g_dns_state = 2;
        g_dns_done_gen = my;
        sceKernelCpuResumeIntr(irq);
        return 0;
    }
    {
        unsigned irq = sceKernelCpuSuspendIntr();
        g_dns_state = 3;
        g_dns_done_gen = my;
        sceKernelCpuResumeIntr(irq);
    }
    return -1;
}

static int dns_worker_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        if (sceKernelWaitSema(g_dns_wake, 1, NULL) < 0) break;
        if (g_dns_state != 1) continue;
        (void)dns_worker_step();
    }
    return 0;
}

static int dns_worker_start(void)
{
    SceUID th;

    if (g_dns_wake < 0) {
        g_dns_wake = sceKernelCreateSema("melody_dns", 0, 0, 1, NULL);
        if (g_dns_wake < 0) return -1;
    }
    if (g_dns_workers >= DNS_WORKER_MAX) return -1;
    th = sceKernelCreateThread("melody_dnsw", dns_worker_main, 0x1c, DNS_WORKER_STACK,
                               PSP_THREAD_ATTR_USER, NULL);
    if (th < 0) return -1;
    if (sceKernelStartThread(th, 0, NULL) < 0) { sceKernelDeleteThread(th); return -1; }
    ++g_dns_workers;
    return 0;
}

static int dns_submit(const char *host)
{
    int gen;

    if (g_dns_workers == 0 && dns_worker_start() < 0) return -1;
    snprintf(g_dns_host, sizeof(g_dns_host), "%s", host);
    gen = ++g_dns_gen;
    g_dns_state = 1;
    sceKernelSignalSema(g_dns_wake, 1);
    return gen;
}

/* 0 = success and *ip set; -1 = failed/timed out. Never blocks past deadline_ms. */
static int dns_wait_job(int gen, int deadline_ms, unsigned int *ip)
{
    unsigned t0 = sceKernelGetSystemTimeLow();

    for (;;) {
        if (g_dns_done_gen == gen) {
            if (g_dns_state != 2) return -1;
            *ip = g_dns_ip;
            return 0;
        }
        if (aborted()) return -1;
        if ((int)((sceKernelGetSystemTimeLow() - t0) / 1000) >= deadline_ms) return -1;
        sceKernelDelayThread(5 * 1000);
    }
}

static int dns_resolve_bounded(const char *host, unsigned int *ip)
{
    int gen = dns_submit(host);

    if (gen < 0) return -1;
    return dns_wait_job(gen, DNS_JOB_DEADLINE_MS, ip);
}

static int dns_lookup_raw(const char *host, unsigned int *ip, int *from_cache)
{
    struct in_addr address;
    unsigned int t0 = sceKernelGetSystemTimeLow();
    unsigned int resolved = 0;
    int result;
    int ms;

    if (inet_aton(host, &address)) {
        *ip = ip_pack((const unsigned char *)&address.s_addr);
        if (from_cache) *from_cache = 0;
        return 0;
    }

    /* A fresh association advertises GOT_IP before its DNS server is usable; the
     * SDK resolver then blocks for 11-46s waiting for it. Check the config first
     * (bounded), then resolve on a dedicated worker that this thread can abandon
     * after DNS_JOB_DEADLINE_MS. A stuck resolver must never freeze playback. */
    if (dns_wait_ready(2000) < 0) {
        if (dns_fallback_ip(host, ip)) {
            mlog("dns use fallback early for %s", host);
            if (from_cache) *from_cache = 1;
            return 0;
        }
    }
    result = dns_resolve_bounded(host, &resolved);
    ms = (int)((sceKernelGetSystemTimeLow() - t0) / 1000);
    mlog("dns done ms=%d result=%d host=%s state=%d cancelled=%d", ms, result, host,
         net_apctl_state(), aborted());
    if (result < 0) {
        if (dns_fallback_ip(host, ip)) {
            mlog("dns use fallback ip for %s", host);
            if (from_cache) *from_cache = 1;
            return 0;
        }
        errno = ETIMEDOUT;
        return -1;
    }

    *ip = resolved;
    if (from_cache) *from_cache = 0;
    return 0;
}

/* from_cache: 1 = 用的是缓存里的地址（连不上可以作废重查） */
/* 系统 DNS 解不出来时的纯 HTTP 解析器：按 IP 直连，不需要域名解析。
 * DNSPod 的 /d 接口返回 "1.2.3.4" 或 "1.2.3.4,ttl" 或 "1.2.3.4;5.6.7.8"。 */
static volatile int g_dns_http_busy;
/* SDK 解析连续失败几次后，先走 HTTP 备用解析，省掉每次 2.5s 的超时等待。
 * 成功一次就清零，避免在网络恢复后仍绕开系统 DNS。 */
static volatile int g_dns_sdk_fails;

static int dns_http_query(const char *host, unsigned int *ip)
{
    char url[256], body[128], addr[24];
    struct in_addr parsed;
    char err[48];
    int len = 0, n = 0;

    if (!host || !host[0] || !ip || strlen(host) > 120) return 0;
    if (g_dns_http_busy) return 0;       /* 递归保护：这个请求本身也走 tcp_connect */
    g_dns_http_busy = 1;
    snprintf(url, sizeof(url), "http://119.29.29.29/d?dn=%s&ttl=1", host);
    if (http_get(url, body, (int)sizeof(body) - 1, &len, 4000, err, sizeof(err)) < 0 ||
        len <= 0) {
        g_dns_http_busy = 0;
        return 0;
    }
    g_dns_http_busy = 0;
    body[len] = 0;
    while (n < (int)sizeof(addr) - 1 && body[n] && body[n] != ',' && body[n] != ';' &&
           body[n] != ' ' && body[n] != '\r' && body[n] != '\n') {
        addr[n] = body[n];
        ++n;
    }
    addr[n] = 0;
    if (n < 7 || !inet_aton(addr, &parsed)) return 0;
    *ip = ip_pack((const unsigned char *)&parsed.s_addr);
    return 1;
}

static int dns_lookup(const char *host, unsigned int *ip, int *from_cache)
{
    unsigned int now = dns_now();
    int stale_i = -1;
    int result;

    if (aborted()) goto dns_cancelled;

    /* 1) 缓存里有效就直接用 */
    for (int i = 0; i < g_dns_n; ++i) {
        if (!strcmp(g_dns[i].host, host)) {
            int ok;

            if (now == 0) ok = (g_dns[i].ts != 0);   /* RTC 不可信：不过期 */
            else ok = (g_dns[i].ts != 0 && now >= g_dns[i].ts &&
                       now - g_dns[i].ts < DNS_TTL_SEC);

            if (ok) {
                *ip = g_dns[i].ip;
                if (from_cache) *from_cache = 1;
                return 0;
            }
            if (g_dns[i].ip) stale_i = i;
            break;
        }
    }

    /* 2) 有过期地址就先用，别为了「刷新」白等 400~900ms。
     *    以前每次 DNS 失败都要先裸查两次（中间还 400ms 睡眠）才肯用旧地址，
     *    封面/评论一多，光 DNS 就能把加载拖慢好几秒。连不上时
     *    tcp_connect() 会把这条记录作废并立刻重查，不影响正确性。 */
    if (stale_i >= 0) {
        mlog("dns use stale %s", host);
        *ip = g_dns[stale_i].ip;
        if (from_cache) *from_cache = 1;
        return 0;
    }

    /* Observe cancellation around the bounded synchronous SDK resolver call. */
    if (aborted()) goto dns_cancelled;
    /* 这个网络里系统 DNS 已经证明解析不了 CDN 域名：别再为每首歌等一次超时。 */
    if (g_dns_sdk_fails >= 2 && dns_http_query(host, ip)) {
        mlog("dns http first %s", host);
        dns_store(host, *ip);
        return 0;
    }
    result = dns_lookup_raw(host, ip, from_cache);
    if (aborted()) goto dns_cancelled;
    if (result == 0) {
        g_dns_sdk_fails = 0;
        dns_store(host, *ip);
        return 0;
    }
    /* 3) 真解析：失败就等 400ms 再来一次（PSP 的 WLAN 会掉进省电模式） */
    sceKernelDelayThread(400 * 1000);
    if (aborted()) goto dns_cancelled;
    result = dns_lookup_raw(host, ip, from_cache);
    if (aborted()) goto dns_cancelled;
    if (result == 0) {
        g_dns_sdk_fails = 0;
        dns_store(host, *ip);
        return 0;
    }
    if (g_dns_sdk_fails < 1000) ++g_dns_sdk_fails;

    /* 3.5) 系统 DNS 解不出来时的纯 HTTP 兜底。
     * 现场日志：m7/m701/m801.music.126.net 每次都撞 2.5s 上限（运营商 DNS 对这批
     * CDN 域名超时），缓存里的歌能放、没缓存的全报"网络连接失败"。
     * 这个接口按 IP 直连（119.29.29.29），本身不需要域名解析，返回 "ip[,ttl]"。 */
    if (dns_http_query(host, ip)) {
        mlog("dns http ok %s", host);
        dns_store(host, *ip);
        return 0;
    }

    /* 4) 内置兜底 IP */
    if (dns_fallback_ip(host, ip)) {
        mlog("dns use fallback ip for %s", host);
        if (from_cache) *from_cache = 1;
        return 0;
    }

    return -1;

dns_cancelled:
    errno = ECANCELED;
    return -1;
}

int net_manual_connect(int profile,int seconds)
{
 if(!g_net_ok||profile<=0||!net_wlan_switch())return -1;
 g_auto_association_owned=0;
 if(g_reconnecting)return -4;
 g_reconnecting=1;net_drop_idle();
 memset(g_dns,0,sizeof(g_dns));g_dns_n=0;dns_save();
 int state=net_apctl_state();
 if(state!=PSP_NET_APCTL_STATE_DISCONNECTED){
  int result=sceNetApctlDisconnect();mlog("net manual: disconnect result=0x%08X state=%d",result,state);
  for(int i=0;i<40;++i){if(aborted()){g_reconnecting=0;return -2;}state=net_apctl_state();if(state==PSP_NET_APCTL_STATE_DISCONNECTED)break;sceKernelDelayThread(50000);}
  if(state!=PSP_NET_APCTL_STATE_DISCONNECTED){g_reconnecting=0;mlog("net manual: disconnect deadline state=%d",state);return -3;}
 }
 g_last_reconn_us=0;
 if(aborted()){g_reconnecting=0;return -2;}
 int result=net_apctl_connect(profile,seconds);g_reconnecting=0;
 mlog("net manual: association result=%d state=%d",result,net_apctl_state());return result;
}

static void dns_expire(const char *host)
{
    for (int i = 0; i < g_dns_n; ++i) {
        if (!strcmp(g_dns[i].host, host)) {
            /* A failed address must not re-enter the stale-while-connect path. */
            g_dns[i].ts = 0;
            g_dns[i].ip = 0;
            return;
        }
    }
}

/** 连接阶段的全局锁：DNS / connect / 读响应头 一次只允许一个线程做。
 *  稳态收数据（recv）不加锁，所以多连接下载不受影响。 */
static int g_conn_lock = -1;

static int conn_lock(void)
{
    if (g_conn_lock < 0)
        g_conn_lock = sceKernelCreateSema("melody_conn", 0, 1, 1, NULL);
    if (g_conn_lock >= 0) {
        while (!aborted()) {
            SceUInt wait = 10000;
            if (g_conn_lock < 0) return 0;
            if (sceKernelWaitSema(g_conn_lock, 1, &wait) >= 0) return 1;
            /* A reset/deleted semaphore may fail immediately instead of waiting. */
            sceKernelDelayThread(1000);
        }
        return 0;
    }
    return 1;
}

static void conn_unlock(void)
{
    if (g_conn_lock >= 0) sceKernelSignalSema(g_conn_lock, 1);
}

/* ------------------------------------------------------------------ */
/* socket                                                              */
/* ------------------------------------------------------------------ */

static void set_timeouts(int sock, int recv_ms, int send_ms)
{
    struct timeval tv;

    tv.tv_sec = recv_ms / 1000;
    tv.tv_usec = (recv_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    tv.tv_sec = send_ms / 1000;
    tv.tv_usec = (send_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* libcglue copies sceNetInetGetErrno verbatim. JPCSP uses newlib numbers;
 * accept the encoded SCE errno form too, only on PSP. Never rewrite errno. */
static int connect_errno_is(int value, int code)
{
    if (value == code) return 1;
#ifdef __PSP__
    if ((unsigned int)value == (0x80010000u | (unsigned int)code)) return 1;
#endif
    return 0;
}

/* conn_lock serializes this temporary owner. Publish a freshly allocated fd
 * to Http before any blocking/polling call, so forced API teardown can find it. */
static Http *g_connect_http;

static int try_connect(unsigned int packed, int port, int timeout_ms, int rcvbuf)
{
    struct sockaddr_in sa;
    unsigned char b[4];
    int sock, saved_errno = 0, one = 1, pending_logged = 0;
    const char *stage = "socket";
    unsigned int t0 = sceKernelGetSystemTimeLow();
    /* Bound multiplication and unsigned low-word elapsed arithmetic (< one wrap).
     * The budget is per TCP attempt; DNS and the existing outer retry are separate. */
    unsigned int budget = (unsigned int)(timeout_ms <= 0 ? 6000 :
                                        timeout_ms > 60000 ? 60000 : timeout_ms) * 1000u;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        int first_error = errno;
        net_drop_idle();
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            saved_errno = errno;
            mlog("net tcp stage=socket port=%d result=-1 errno=%d first=%d",
                 port, saved_errno, first_error);
            errno = saved_errno;
            return -1;
        }
        mlog("net socket recovered after closing idle pool");
    }
    if (g_connect_http) g_connect_http->sock = sock;
    stage = "fd_range";
    if (sock >= FD_SETSIZE) { saved_errno = EMFILE; goto connect_fail; }
    if (aborted()) { saved_errno = ECANCELED; goto connect_fail; }
    stage = "SO_NONBLOCK";
    if (setsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one)) < 0) {
        saved_errno = errno;
        goto connect_fail;
    }
    if (rcvbuf > 0 && setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        int option_errno = errno;
        mlog("net tcp stage=SO_RCVBUF fd=%d request=%d errno=%d (default retained)",
             sock, rcvbuf, option_errno);
    }
    set_timeouts(sock, 800, 6000);

    ip_unpack(packed, b);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, b, 4);

    if (aborted()) { saved_errno = ECANCELED; goto connect_fail; }
    if ((unsigned int)(sceKernelGetSystemTimeLow() - t0) >= budget) {
        saved_errno = ETIMEDOUT;
        goto connect_fail;
    }
    stage = "connect";
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        saved_errno = errno;
        if (!connect_errno_is(saved_errno, EINPROGRESS) &&
            !connect_errno_is(saved_errno, EALREADY) &&
            !connect_errno_is(saved_errno, EWOULDBLOCK) &&
            !connect_errno_is(saved_errno, EINTR)) goto connect_fail;
        for (;;) {
            fd_set writable, exceptional;
            struct timeval tv;
            unsigned int poll_t0 = sceKernelGetSystemTimeLow();
            unsigned int elapsed = poll_t0 - t0;
            unsigned int slice;
            int result;

            if (aborted()) { saved_errno = ECANCELED; goto connect_fail; }
            if (elapsed >= budget) { saved_errno = ETIMEDOUT; goto connect_fail; }
            slice = budget - elapsed;
            if (slice > 50000u) slice = 50000u;
            FD_ZERO(&writable);
            FD_SET(sock, &writable);
            FD_ZERO(&exceptional);
            FD_SET(sock, &exceptional);
            tv.tv_sec = 0;
            tv.tv_usec = slice;
            /* Use libc select: its fd map and timeval conversion match socket(). */
            stage = "select";
            result = select(sock + 1, NULL, &writable, &exceptional, &tv);
            if (result < 0) {
                saved_errno = errno;
                if (connect_errno_is(saved_errno, EINTR)) continue;
                goto connect_fail;
            }
            if (aborted()) { saved_errno = ECANCELED; goto connect_fail; }
            if ((unsigned int)(sceKernelGetSystemTimeLow() - t0) >= budget) {
                saved_errno = ETIMEDOUT;
                goto connect_fail;
            }
            if (result > 0 && (FD_ISSET(sock, &writable) || FD_ISSET(sock, &exceptional))) {
                int error = 0;
                socklen_t len = sizeof(error);
                int wr = !!FD_ISSET(sock, &writable);
                int exc = !!FD_ISSET(sock, &exceptional);
                int optret, opt_errno;
                const char *reason = NULL;
                stage = "SO_ERROR";
                optret = getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
                opt_errno = optret < 0 ? errno : 0;
                if (optret < 0) { saved_errno = opt_errno; reason = "getsockopt_failed"; }
                else if (len != sizeof(error)) { saved_errno = EIO; reason = "optlen"; }
                else if (error != 0) { saved_errno = error; reason = "socket_error"; }
                else if (!wr) {
                    /* Exceptional readiness is not itself proof of failure or
                     * success. Require the exact requested connected IPv4 peer;
                     * never override a failed SO_ERROR query or actual error. */
                    struct sockaddr_in peer;
                    socklen_t peer_len = sizeof(peer);
                    int peerret, peer_errno, match;
                    memset(&peer, 0, sizeof(peer));
                    peerret = getpeername(sock, (struct sockaddr *)&peer, &peer_len);
                    peer_errno = peerret < 0 ? errno : 0;
                    match = peerret == 0 && peer_len == sizeof(peer) &&
                            peer.sin_family == AF_INET && peer.sin_port == sa.sin_port &&
                            memcmp(&peer.sin_addr, &sa.sin_addr, sizeof(sa.sin_addr)) == 0;
                    if (peerret < 0 && connect_errno_is(peer_errno, ENOTCONN)) {
                        /* Some PSP stacks signal exception while the handshake is
                         * still pending. Zero SO_ERROR plus ENOTCONN proves neither
                         * success nor failure; retain this attempt's original budget. */
                        unsigned int now, spent, remaining, pause;
                        if (!pending_logged) {
                            mlog("net tcp ready fd=%d reason=exception_pending rawret=%d len=%u error=%d writable=%d exc=%d peer_errno=%d",
                                 sock, optret, (unsigned)len, error, wr, exc, peer_errno);
                            pending_logged = 1;
                        }
                        stage = "peer_pending";
                        now = sceKernelGetSystemTimeLow();
                        spent = now - poll_t0;
                        elapsed = now - t0;
                        if (aborted()) { saved_errno = ECANCELED; goto connect_fail; }
                        if (elapsed >= budget) { saved_errno = ETIMEDOUT; goto connect_fail; }
                        /* Level-triggered exceptions may return immediately. Give
                         * other threads the unused slice, capped by the deadline. */
                        remaining = budget - elapsed;
                        pause = spent < slice ? slice - spent : 0;
                        if (pause > remaining) pause = remaining;
                        if (pause) sceKernelDelayThread(pause);
                        continue;
                    }
                    mlog("net tcp peer fd=%d ret=%d errno=%d len=%u family=%d port=%u match=%d",
                         sock, peerret, peer_errno, (unsigned)peer_len,
                         (int)peer.sin_family, (unsigned)ntohs(peer.sin_port), match);
                    if (!match) { saved_errno = EIO; reason = "exception_no_peer"; }
                    else mlog("net tcp ready fd=%d reason=peer_confirmed rawret=%d len=%u error=%d writable=%d exc=%d",
                              sock, optret, (unsigned)len, error, wr, exc);
                }
                if (reason) {
                    mlog("net tcp ready fd=%d reason=%s rawret=%d opt_errno=%d len=%u error=%d writable=%d exc=%d select=%d",
                         sock, reason, optret, opt_errno, (unsigned)len, error, wr, exc, result);
                    goto connect_fail;
                }
                break;
            }
        }
    }
    if (aborted()) { saved_errno = ECANCELED; goto connect_fail; }
    if ((unsigned int)(sceKernelGetSystemTimeLow() - t0) >= budget) {
        saved_errno = ETIMEDOUT;
        goto connect_fail;
    }
    mlog("net tcp port=%d ms=%u result=0 errno=0", port,
         (unsigned int)(sceKernelGetSystemTimeLow() - t0) / 1000);

    {
        /* 只打一次：真机上 SO_RCVBUF 可能被协议栈夹到更小，排查要靠这个数 */
        static int logged;

        if (!logged && rcvbuf > 0) {
            int got = 0;
            socklen_t gl = sizeof(got);

            if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &got, &gl) == 0) {
                mlog("socket rcvbuf request=%d effective=%d", rcvbuf, got);
                logged = 1;
            }
        }
    }
    return sock;

connect_fail:
    mlog("net tcp port=%d ms=%u result=-1 errno=%d stage=%s fd=%d", port,
         (unsigned int)(sceKernelGetSystemTimeLow() - t0) / 1000, saved_errno, stage, sock);
    if (g_connect_http) g_connect_http->sock = -1;
    close(sock);
    errno = saved_errno;
    return -1;
}

static int tcp_connect(const char *host, int port, int timeout_ms,
                       int rcvbuf, char *err, int errlen)
{
    unsigned int ip = 0;
    unsigned int dns_t0;
    int from_cache = 0;
    int sock;
    int saved_errno;

    if (aborted()) goto tcp_cancelled;
    if (!g_net_ok) {
        if (err) snprintf(err, (size_t)errlen, "network not initialized");
        return -1;
    }

    /* Direct HTTP users (updates, announcements, skins) do not pass through
     * api fetch()'s reconnect path. Kick association before DNS, without waiting
     * or interrupting an association already in progress. A later request retries. */
    {
        int state = net_apctl_state();
        if (state != PSP_NET_APCTL_STATE_GOT_IP) {
            char local_ip[32] = "";
            net_current_ip(local_ip, sizeof(local_ip));
            net_reconnect_poll();
            if (aborted()) goto tcp_cancelled;
            mlog("net link unavailable host=%s state=%d profile=%d ip=%s errno=%d",
                 host, state, g_last_profile, local_ip[0] ? local_ip : "none", EADDRNOTAVAIL);
            if (err) snprintf(err, (size_t)errlen,
                              "WiFi reconnecting (state=%d); retry", state);
            errno = EADDRNOTAVAIL;
            return -1;
        }
    }

    dns_t0 = sceKernelGetSystemTimeLow();
    if (dns_lookup(host, &ip, &from_cache) < 0) {
        if (aborted() || connect_errno_is(errno, ECANCELED)) goto tcp_cancelled;
        mlog("net dns host=%s ms=%u result=fail", host,
             (unsigned)(sceKernelGetSystemTimeLow() - dns_t0) / 1000);
        if (err) snprintf(err, (size_t)errlen, "cannot resolve %s", host);
        return -1;
    }
    mlog("net dns host=%s ms=%u cached=%d result=ok", host,
         (unsigned)(sceKernelGetSystemTimeLow() - dns_t0) / 1000, from_cache);

    if (aborted()) goto tcp_cancelled;
    sock = try_connect(ip, port, timeout_ms, rcvbuf);
    if (sock >= 0) return sock;
    if (connect_errno_is(errno, EADDRNOTAVAIL)) {
        char local_ip[32] = "";
        int address_errno = errno;
        net_current_ip(local_ip, sizeof(local_ip));
        mlog("net address unavailable host=%s state=%d profile=%d ip=%s",
             host, net_apctl_state(), g_last_profile, local_ip[0] ? local_ip : "none");
        errno = address_errno;
    }
    if (aborted() || connect_errno_is(errno, ECANCELED)) goto tcp_cancelled;

    /* 缓存里的地址连不上（换了 CDN 节点 / IP 过期）：作废重查一次 */
    if (from_cache) {
        mlog("connect to cached ip failed, re-resolve %s", host);
        dns_expire(host);
        dns_t0 = sceKernelGetSystemTimeLow();
        int dns_result = dns_lookup(host, &ip, NULL);
        if (aborted() || (dns_result < 0 && connect_errno_is(errno, ECANCELED))) goto tcp_cancelled;
        mlog("net dns refresh host=%s ms=%u result=%d", host,
             (unsigned)(sceKernelGetSystemTimeLow() - dns_t0) / 1000, dns_result);
        if (dns_result == 0) {
            sock = try_connect(ip, port, timeout_ms, rcvbuf);
            if (sock >= 0) return sock;
        }
    } else {
        /* 真机 WiFi 偶尔第一次握手失败，再试一次 */
        sceKernelDelayThread(120 * 1000);
        if (aborted()) goto tcp_cancelled;
        sock = try_connect(ip, port, timeout_ms, rcvbuf);
        if (sock >= 0) {
            mlog("connect retry ok %s:%d", host, port);
            return sock;
        }
    }

    if (aborted() || connect_errno_is(errno, ECANCELED)) goto tcp_cancelled;
    saved_errno = errno;
    if (err) snprintf(err, (size_t)errlen, "cannot connect %s:%d", host, port);
    mlog("connect fail %s:%d errno=%d", host, port, saved_errno);
    errno = saved_errno;
    return -1;

tcp_cancelled:
    if (err) snprintf(err, (size_t)errlen, "aborted");
    errno = ECANCELED;
    return -1;
}

int net_probe_host(const char *host, int port, int timeout_ms)
{
    int sock;

    if (!conn_lock()) return -1;
    sock = tcp_connect(host, port, timeout_ms, 0, NULL, 0);
    conn_unlock();

    if (sock < 0) return -1;
    close(sock);
    return 0;
}

/* >0 字节；0 对端关闭；-1 暂时没数据；-2 出错
 *
 * 一律用 MSG_DONTWAIT 主动轮询，不靠 SO_RCVTIMEO：
 * PSP/PPSSPP 的 recv 在超时之后会返回 0（跟对端关闭一模一样），
 * 大响应会被莫名截断；而且多个连接要靠轮询才能并发。
 */
static volatile unsigned net_rx_bytes;
static unsigned net_rate_bytes,net_rate_time;
static int net_rate_kbps,net_peak_kbps;
static void net_received(int bytes){if(bytes>0){unsigned irq=sceKernelCpuSuspendIntr();net_rx_bytes+=(unsigned)bytes;sceKernelCpuResumeIntr(irq);}}
void net_speed_tick(void){
    unsigned now=sceKernelGetSystemTimeLow(),bytes=net_rx_bytes;
    if(!net_rate_time){net_rate_time=now;net_rate_bytes=bytes;return;}
    unsigned elapsed=now-net_rate_time;
    if(elapsed>=500000u){net_rate_kbps=(int)((unsigned long long)(bytes-net_rate_bytes)*1000000u/elapsed/1024u);if(net_rate_kbps>net_peak_kbps)net_peak_kbps=net_rate_kbps;net_rate_time=now;net_rate_bytes=bytes;}
}
int net_speed_kbps(void){return net_rate_kbps;}
int net_peak_speed_kbps(void){return net_peak_kbps;}

static int recv_wait(int sock, char *buf, int len, int idle_ms)
{
    unsigned int t0 = sceKernelGetSystemTimeLow();

    if (idle_ms <= 0) {
        /* 多连接轮询：一次非阻塞尝试就返回，绝不占住线程 */
        int n = recv(sock, buf, len, MSG_DONTWAIT);

        if (n > 0) {net_received(n);return n;}
        if (n == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -2;
    }

    /* 小请求（JSON / HTTP 头）：老老实实阻塞读，socket 自身有 800ms 超时，
     * 这样系统调用次数最少，也不会和下载线程抢着轮询 */
    for (;;) {
        int n;
        if (aborted()) return -3;
        n = recv(sock, buf, len, MSG_DONTWAIT);

        if (n > 0) {net_received(n);return n;}
        if (n == 0) return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -2;
        if ((int)((sceKernelGetSystemTimeLow() - t0) / 1000) >= idle_ms) return -1;
        /* socket 是 SO_NONBLOCK 的，EAGAIN 会立刻回来：让出 CPU，别空转 */
        sceKernelDelayThread(3 * 1000);
    }
}

static int send_all(int sock, const char *buf, int len)
{
    int sent = 0;
    unsigned int start = sceKernelGetSystemTimeLow();

    while (sent < len) {
        int n;
        if (aborted() || sceKernelGetSystemTimeLow() - start > 8000000u) return -1;
        n = send(sock, buf + sent, len - sent, MSG_DONTWAIT);
        if (n > 0) { sent += n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            sceKernelDelayThread(10 * 1000);
            continue;
        }
        return -1;
    }
    return sent;
}

/* 往 Http 内部缓冲补数据：>0 字节，0 关闭，-1 超时，-2 错 */
static int fill(Http *h, int idle_ms)
{
    int n;

    if (h->bpos < h->blen) return h->blen - h->bpos;

    h->bpos = 0;
    h->blen = 0;

    n = recv_wait(h->sock, (char *)h->buf, sizeof(h->buf), idle_ms);
    if (n > 0) {
        h->blen = n;
        h->bytes_total += n;
        return n;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

static SceUID post_owner=-1;
static const char *post_body;
static int http_connect_raw(Http *h, const char *url, long long range_from,
                             long long range_to, int timeout_ms, int body_timeout,
                             char *err, int errlen, char *location, int loclen)
{
    char req[2600];
    char hdr[HDR_MAX];
    int n;

    if (loclen > 0) location[0] = 0;

    n = parse_url(url, h->host, sizeof(h->host), &h->port, h->path, sizeof(h->path));
    if (n == -2) {
        snprintf(err, (size_t)errlen, "this link is https, PSP cannot use it");
        return -1;
    }
    if (n < 0) {
        snprintf(err, (size_t)errlen, "bad http url");
        return -1;
    }

    h->range_from = range_from;
    h->body_bytes = 0;
    h->bytes_total = 0;

    /* 没有 Range 的 GET 才复用连接：分片下载/音频流必须独占 socket */
    h->keep_alive = g_want_keepalive && !h->no_reuse && range_from == 0 && range_to < 0;
    h->reused = 0;
    h->sock = h->keep_alive ? ka_take(h->host, h->port) : -1;
    if (h->sock >= 0) {
        h->reused = 1;
    } else {
        /* 池子里的空闲连接只吃小请求（JSON/封面），接收窗口给 24KB 就够：
         * 每条 64KB 的话，4 条空闲连接就能把 PSP 的 768KB 网络池吃掉三分之一。 */
        g_connect_http = h;
        h->sock = tcp_connect(h->host, h->port, timeout_ms,
                              h->keep_alive ? 24 * 1024 : 64 * 1024, err, errlen);
        g_connect_http = NULL;
        if (h->sock < 0) return -2;
    }

    int posting=post_body&&post_owner==sceKernelGetThreadId();
    if(posting){
        n=snprintf(req,sizeof(req),"POST %s HTTP/1.0\r\nHost: %s\r\n%sUser-Agent: MELODY-PSP\r\nReferer: http://music.163.com/\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",h->path,h->host,cookie_hdr(h->host),(unsigned)strlen(post_body));
    }else if (range_from > 0 || range_to >= 0) {
        if (range_to >= range_from) {
            n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.0\r\n"
                         "Host: %s\r\n"
                         "%s"
                         "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
                         "Accept: */*\r\n"
                         "Referer: http://music.163.com/\r\n"
                         "Accept-Encoding: identity\r\n"
                         "Range: bytes=%lld-%lld\r\n"
                         "Connection: close\r\n\r\n",
                         h->path, h->host, cookie_hdr(h->host), range_from, range_to);
        } else {
            n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.0\r\n"
                         "Host: %s\r\n"
                         "%s"
                         "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
                         "Accept: */*\r\n"
                         "Referer: http://music.163.com/\r\n"
                         "Accept-Encoding: identity\r\n"
                         "Range: bytes=%lld-\r\n"
                         "Connection: close\r\n\r\n",
                         h->path, h->host, cookie_hdr(h->host), range_from);
        }
    } else {
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "%s"
                     "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
                     "Accept: */*\r\n"
                     "Referer: http://music.163.com/\r\n"
                     "Accept-Encoding: %s\r\n"
                     "Connection: %s\r\n\r\n",
                     h->path, h->host, cookie_hdr(h->host),
                     g_want_gzip ? "gzip, identity" : "identity",
                     h->keep_alive ? "keep-alive" : "close");
    }

    if(n<=0||n>=(int)sizeof(req)){snprintf(err,(size_t)errlen,"HTTP request headers too long");return -3;}
    if (send_all(h->sock, req, n) != n) {
        snprintf(err, (size_t)errlen, "send failed");
        return -3;
    }

    if(posting&&send_all(h->sock,post_body,(int)strlen(post_body))!=(int)strlen(post_body)){snprintf(err,(size_t)errlen,"POST send failed");return -3;}
    {
        char *sep = NULL;
        int body_off;

        h->blen = 0;
        h->bpos = 0;
        h->buf[0] = 0;

        /* 直接在 h->buf 里攒头部，别把正文丢进一个更小的临时缓冲 ——
         * 一个 recv 可能带回 16KB（头 + 正文），以前那种写法会把
         * 塞不进头部缓冲的正文字节直接丢掉，大响应就少一截。 */
        for (;;) {
            int r;
            int room;

            if (h->blen >= (int)sizeof(h->buf) - 2) break;
            room = (int)sizeof(h->buf) - 1 - h->blen;

            if (aborted()) { snprintf(err, (size_t)errlen, "aborted"); return -4; }
            r = recv_wait(h->sock, (char *)h->buf + h->blen, room, timeout_ms);
            if (r > 0) {
                h->blen += r;
                h->bytes_total += r;
                h->buf[h->blen] = 0;
                sep = strstr((char *)h->buf, "\r\n\r\n");
                if (sep) break;
                continue;
            }
            if (r == 0) { snprintf(err, (size_t)errlen, "connection closed"); return -6; }
            if (r == -1) { snprintf(err, (size_t)errlen, "timeout waiting reply"); return -4; }
            snprintf(err, (size_t)errlen, "recv failed");
            return -5;
        }

        if (!sep) {
            snprintf(err, (size_t)errlen, "bad http header");
            return -7;
        }

        body_off = (int)(sep + 4 - (char *)h->buf);

        /* 头部复制一份出来解析（hdr 里已经被 NUL 截断也没关系） */
        {
            int hl = body_off < (int)sizeof(hdr) - 1 ? body_off : (int)sizeof(hdr) - 1;
            memcpy(hdr, h->buf, (size_t)hl);
            hdr[hl] = 0;
        }

        /* 头后面已经读到的正文留在缓冲开头，一个字节都不丢 */
        {
            int extra = h->blen - body_off;
            if (extra > 0) memmove(h->buf, h->buf + body_off, (size_t)extra);
            h->blen = extra > 0 ? extra : 0;
            h->bpos = 0;
        }

        if (sscanf(hdr, "HTTP/%*[0-9.] %d", &h->status) != 1) {
            snprintf(err, (size_t)errlen, "not a http reply");
            return -8;
        }

        h->chunked = 0;
        h->chunk_left = 0;
        h->chunk_state = 0;
        h->chunk_size = 0;
        h->chunk_digits = 0;
        h->chunk_ext = 0;
        h->chunk_cr = 0;
        h->chunk_line_len = 0;
        h->chunk_trailer_bytes = 0;
        h->finished = 0;
        h->remaining = -1;
        h->range_start = h->range_end = h->range_total = -1;
        h->content_length = -1;
        h->gzip = 0;

        if (ci_strstr(hdr, "Transfer-Encoding: chunked")) h->chunked = 1;
        /* 服务端压过的正文要解压后才能交给 JSON 解析器 */
        if (ci_strstr(hdr, "Content-Encoding: gzip")) h->gzip = 1;

        {
            char *cl = header_value(hdr, "Content-Length");
            if (cl) {
                h->content_length = atoll(cl);
                h->remaining = h->content_length;
            }
        }

        {
            char *cr = header_value(hdr, "Content-Range");

            parse_content_range(h, cr);
        }

        if (h->status >= 300 && h->status < 400) {
            char *loc = header_value(hdr, "Location");
            if (loc && *loc) snprintf(location, (size_t)loclen, "%s", loc);
        }
    }

    /* 正文阶段用短超时（下载线程 2ms），这样 recv 不会把线程按住几十秒 */
    http_set_timeout(h, body_timeout);

    mlog("http %d len=%lld chunked=%d from=%lld %s", h->status, h->content_length,
         h->chunked, range_from, h->path);
    return 0;
}

int http_set_timeout(Http *h, int ms)
{
    /* 读我们靠 MSG_DONTWAIT 轮询，这里只记下来（socket 的 RCVTIMEO 只当兜底） */
    if (!h || h->sock < 0) return -1;
    if (ms < 1) ms = 1;
    h->body_timeout = ms;
    return 0;
}

static int http_connect_once(Http *h, const char *url, long long range_from,
                             long long range_to, int timeout_ms, int body_timeout,
                             char *err, int errlen, char *location, int loclen)
{
    int r;

    if (!conn_lock()) { snprintf(err, (size_t)errlen, "aborted"); return -4; }
    r = http_connect_raw(h, url, range_from, range_to, timeout_ms, body_timeout,
                         err, errlen, location, loclen);
    conn_unlock();

    /* 复用的空闲连接可能已经被服务端关了（keep-alive 超时）：
     * 丢掉它，换一条新连接重来一次。只重试一次，避免死循环。 */
    if (r < 0 && h->reused) {
        mlog("ka stale, reconnect %s", h->host);
        h->reused_broken = 1;
        http_close(h);
        h->no_reuse = 1;
        h->reused_broken = 0;
        if (!conn_lock()) { snprintf(err, (size_t)errlen, "aborted"); return -4; }
        r = http_connect_raw(h, url, range_from, range_to, timeout_ms, body_timeout,
                             err, errlen, location, loclen);
        conn_unlock();
    }
    return r;
}

static int http_open_full(Http *h, const char *url, long long range_from, long long range_to,
                          int timeout_ms, int body_timeout, char *err, int errlen)
{
    char cur[1200];
    int redirects = 0;

    memset(h, 0, sizeof(*h));
    h->sock = -1;
    g_set_cookie[0] = 0;

    if (err && errlen > 0) err[0] = 0;
    snprintf(cur, sizeof(cur), "%s", url);

    for (;;) {
        char location[900];
        int r = http_connect_once(h, cur, range_from, range_to, timeout_ms, body_timeout,
                                  err, errlen, location, sizeof(location));

        if (r < 0) {
            http_close(h);
            return -1;
        }

        if (h->status >= 300 && h->status < 400 && h->status != 304) {
            http_close(h);

            if (!location[0]) {
                snprintf(err, (size_t)errlen, "HTTP %d without Location", h->status);
                return -2;
            }
            if (!strncasecmp(location, "https://", 8)) {
                snprintf(err, (size_t)errlen, "server wants https");
                return -3;
            }
            if (++redirects > MAX_REDIRECTS) {
                snprintf(err, (size_t)errlen, "too many redirects");
                return -4;
            }

            abs_url(cur, location, cur, sizeof(cur));
            mlog("redirect -> %s", cur);
            continue;
        }

        if (h->status != 200 && h->status != 206) {
            snprintf(err, (size_t)errlen, "HTTP %d", h->status);
            http_close(h);
            return -5;
        }

        /* 扫码登录成功时，服务端用 Set-Cookie 下发 MUSIC_U */
        grab_cookie_pair(h->buf, "MUSIC_U=");
        grab_cookie_pair(h->buf, "__csrf=");

        return 0;
    }
}

int http_open(Http *h, const char *url, long long range_from,
              int timeout_ms, char *err, int errlen)
{
    /* 正文阶段 20ms 没数据就返回，让调用方有机会干别的（界面/中断） */
    return http_open_full(h, url, range_from, -1, timeout_ms, 20, err, errlen);
}

int http_open_range(Http *h, const char *url, long long from, long long to,
                    int timeout_ms, char *err, int errlen)
{
    /* 并行分片下载：每片都要能在一个轮询周期里拿到数据，用 5ms */
    return http_open_full(h, url, from, to, timeout_ms, 5, err, errlen);
}

int http_open_range_cancel(Http *h, const char *url, long long from, long long to,
                           int timeout_ms, volatile int *cancel, int cancel_value,
                           char *err, int errlen)
{
    int r;
    /* The download engine has one worker. Do not assign g_abort_flag here: that
     * pointer belongs to API fetches on a different thread. */
    g_cancel_flag = cancel;
    g_cancel_value = cancel_value;
    g_cancel_owner = sceKernelGetThreadId();
    r = http_open_full(h, url, from, to, timeout_ms, 5, err, errlen);
    if (cancel && *cancel != cancel_value) {
        http_close(h);
        r = -3;
        if (err && errlen > 0) snprintf(err, (size_t)errlen, "aborted");
    }
    g_cancel_owner = -1;
    g_cancel_flag = NULL;
    return r;
}

/* 大文件整包下载（MV）：不进 keep-alive 池，接收窗给 64KB。
 * 池子里的连接只吃小请求、给 24KB 窗；MV 是 11MB 连续流，窗口/RTT 直接限速。 */
int http_open_bulk(Http *h, const char *url, int timeout_ms, char *err, int errlen)
{
    int saved = g_want_keepalive;
    int r;

    g_want_keepalive = 0;
    r = http_open_full(h, url, 0, -1, timeout_ms, 20, err, errlen);
    g_want_keepalive = saved;
    return r;
}

void http_close(Http *h)
{
    if (!h || h->sock < 0) return;

    /* 完整读完的 keep-alive 连接放回池子，下次同一域名直接复用，
     * 省掉 PSP 上最贵的 TCP 握手 + DNS。 */
    if (h->keep_alive && h->finished && !h->reused_broken &&
        h->bpos == h->blen &&
        (h->status == 200 || h->status == 206) && h->sock >= 0) {
        ka_put(h->host, h->port, h->sock);
        h->sock = -1;
        return;
    }
    close(h->sock);
    h->sock = -1;
}

/* 0..255 = byte (including NUL), -1 = timeout, -2 = error, -3 = EOF. */
static int rd_byte(Http *h, int idle_ms)
{
    if (h->bpos >= h->blen) {
        int r = fill(h, idle_ms);
        if (r <= 0) return r == 0 ? -3 : r;
    }
    return (int)h->buf[h->bpos++];
}

enum {
    CHUNK_SIZE, CHUNK_DATA, CHUNK_DATA_CR, CHUNK_DATA_LF,
    CHUNK_TRAILER, CHUNK_ERROR
};
#define CHUNK_LINE_MAX 1024
#define CHUNK_TRAILER_MAX 16384

int http_read(Http *h, char *dst, int len, int idle_ms,
              volatile int *gen, int my_gen, char *err, int errlen)
{
    int out = 0;
    unsigned int t0 = sceKernelGetSystemTimeLow();

    if (!h || h->sock < 0) return -2;
    if (h->finished) return 0;

    if (!h->chunked) {
        while (out < len) {
            int avail;

            if (gen && my_gen >= 0 && *gen != my_gen) return -3;
            if (h->remaining == 0) { h->finished = 1; break; }

            avail = h->blen - h->bpos;
            if (avail <= 0) {
                int r = fill(h, 0);         /* 一次非阻塞尝试 */
                if (r == 0) {
                    /* 有的栈把 SO_RCVTIMEO 超时也报成 0（= 对端关闭），
                     * 跟真正的断连分不开。长度还没读满就先重试几次，
                     * 否则大响应会被莫名截断（榜单 80KB 只收到 76KB 就是这样）。 */
                    if (h->remaining > 0 && h->eof_retry < 6) {
                        h->eof_retry++;
                        sceKernelDelayThread(15 * 1000);
                        continue;
                    }
                    h->finished = 1;
                    break;
                }
                if (r == -2) { snprintf(err, (size_t)errlen, "recv error"); return -2; }
                if (r < 0) {
                    /* 暂时没数据：前面已经有东西就先返回，避免占着线程 */
                    if (out > 0) break;
                    if (idle_ms <= 0) return -1;
                    if ((int)((sceKernelGetSystemTimeLow() - t0) / 1000) >= idle_ms) return -1;
                    sceKernelDelayThread(3000);
                    continue;
                }
                h->eof_retry = 0;
                avail = h->blen - h->bpos;
            }

            {
                int n = len - out;
                if (n > avail) n = avail;
                if (h->remaining > 0 && (long long)n > h->remaining) n = (int)h->remaining;
                memcpy(dst + out, h->buf + h->bpos, (size_t)n);
                h->bpos += n;
                out += n;
                if (h->remaining > 0) h->remaining -= n;
                h->body_bytes += n;
            }
        }
        return out;
    }

    /* 所有 framing 状态都放在 Http：短超时不能丢掉半行、CR 或 trailer。 */
    while (out < len) {
        int c;
        if (gen && my_gen >= 0 && *gen != my_gen) return -3;
        if (h->chunk_state == CHUNK_ERROR) {
            snprintf(err, (size_t)errlen, "bad chunk stream");
            return -2;
        }

        if (h->chunk_state == CHUNK_DATA) {
            int avail = fill(h, 0);
            if (avail == 0) goto chunk_eof;
            if (avail == -2) goto chunk_recv_error;
            if (avail == -1) goto chunk_wait;
            {
                int n = len - out;
                if (n > avail) n = avail;
                if (n > h->chunk_left) n = h->chunk_left;
                memcpy(dst + out, h->buf + h->bpos, (size_t)n);
                h->bpos += n;
                out += n;
                h->chunk_left -= n;
                h->body_bytes += n;
                if (h->chunk_left == 0) h->chunk_state = CHUNK_DATA_CR;
            }
            continue;
        }

        c = rd_byte(h, 0);
        if (c == -3) goto chunk_eof;
        if (c == -2) goto chunk_recv_error;
        if (c == -1) goto chunk_wait;

        if (h->chunk_state == CHUNK_DATA_CR) {
            if (c != '\r') goto chunk_bad_header;
            h->chunk_state = CHUNK_DATA_LF;
            continue;
        }
        if (h->chunk_state == CHUNK_DATA_LF) {
            if (c != '\n') goto chunk_bad_header;
            h->chunk_state = CHUNK_SIZE;
            continue;
        }

        if (++h->chunk_line_len > CHUNK_LINE_MAX) goto chunk_bad_header;
        if (h->chunk_state == CHUNK_TRAILER &&
            ++h->chunk_trailer_bytes > CHUNK_TRAILER_MAX) goto chunk_bad_header;

        if (h->chunk_cr) {
            if (c != '\n') goto chunk_bad_header;
            h->chunk_cr = 0;
            if (h->chunk_state == CHUNK_TRAILER) {
                if (h->chunk_line_len == 2) {
                    h->finished = 1;  /* 只有完整 trailer 的空行才是正文结束 */
                    return out;
                }
            } else {
                if (!h->chunk_digits) goto chunk_bad_header;
                h->chunk_left = h->chunk_size;
                h->chunk_state = h->chunk_size ? CHUNK_DATA : CHUNK_TRAILER;
                h->chunk_size = 0;
                h->chunk_digits = 0;
                h->chunk_ext = 0;
            }
            h->chunk_line_len = 0;
            continue;
        }
        if (c == '\r') {
            h->chunk_cr = 1;
            continue;
        }
        if (c < 32 && c != '\t') goto chunk_bad_header;
        if (c == 127) goto chunk_bad_header;
        if (h->chunk_state == CHUNK_SIZE && h->chunk_ext != 1) {
            int d;
            if (h->chunk_ext == 2) {
                if (c == ';') h->chunk_ext = 1;
                else if (c != ' ' && c != '\t') goto chunk_bad_header;
                continue;
            }
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else if (c == ';' && h->chunk_digits) {
                h->chunk_ext = 1;  /* 扩展内容不参与 size，仍受行长度限制 */
                continue;
            } else if ((c == ' ' || c == '\t') && h->chunk_digits) {
                h->chunk_ext = 2;  /* 扩展分号前的 BWS，后面不能再接 hex */
                continue;
            } else goto chunk_bad_header;
            if (h->chunk_size > (INT_MAX - d) / 16) goto chunk_bad_header;
            h->chunk_size = h->chunk_size * 16 + d;
            h->chunk_digits = 1;
        }
        continue;

chunk_wait:
        if (out > 0) return out;
        if (idle_ms <= 0 ||
            (int)((sceKernelGetSystemTimeLow() - t0) / 1000) >= idle_ms) return -1;
        sceKernelDelayThread(3000);
    }
    return out;

chunk_eof:
    snprintf(err, (size_t)errlen, "connection closed in chunked body");
    goto chunk_fail;
chunk_recv_error:
    snprintf(err, (size_t)errlen, "recv error");
    goto chunk_fail;
chunk_bad_header:
    snprintf(err, (size_t)errlen, "bad chunk header");
chunk_fail:
    h->chunk_state = CHUNK_ERROR;
    h->reused_broken = 1;
    return out > 0 ? out : -2;
}

/* 静态 Http：这个结构体 17KB，放线程栈上会把 32KB 的栈撑爆
 * （表现为随机假死）。用信号量串行化，谁调都安全。 */
static Http g_get_http = { .sock = -1 };
static int g_get_lock = -1;

static int g_last_expect;

int http_get_expect(void) { return g_last_expect; }

/* gzip 正文解压。输入一次 16KB，边收边解，内存只要 zlib 自己的窗口。
 * 返回 0 完整 / -1 超时或中断 / -2 数据损坏或被截断（调用方可以重试）。 */
static int http_inflate(Http *hp, char *out, int outmax, int *outlen,
                        int timeout_ms, char *err, int errlen)
{
    static unsigned char in[16384];
    z_stream zs;
    int total = 0;
    int done = 0;
    int idle = 0;
    unsigned int t0 = sceKernelGetSystemTimeLow();

    *outlen = 0;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {
        snprintf(err, (size_t)errlen, "zlib init");
        return -2;
    }

    for (;;) {
        int n;

        if (done) break;
        if (total >= outmax - 1) { mlog("gz: out full at %d", total); break; }
        if (aborted()) { inflateEnd(&zs); return -1; }

        n = http_read(hp, (char *)in, (int)sizeof(in), 0, NULL, -1, err, errlen);
        if (n == 0) break;                 /* 对端关闭：下面看是否 Z_STREAM_END */
        if (n < 0) {
            if (n == -2) { inflateEnd(&zs); return -2; }
            if ((int)((sceKernelGetSystemTimeLow() - t0) / 1000) > timeout_ms) break;
            if (++idle > 3000) break;
            sceKernelDelayThread(3 * 1000);
            continue;
        }
        idle = 0;

        zs.next_in = in;
        zs.avail_in = (uInt)n;
        while (zs.avail_in > 0) {
            int zr;

            zs.next_out = (Bytef *)(out + total);
            zs.avail_out = (uInt)(outmax - 1 - total);
            zr = inflate(&zs, Z_SYNC_FLUSH);
            total = (int)(zs.next_out - (Bytef *)out);
            if (zr == Z_STREAM_END) { done = 1; break; }
            if (zr != Z_OK && zr != Z_BUF_ERROR) {
                mlog("gz err %d after %d", zr, total);
                inflateEnd(&zs);
                return -2;
            }
            if (zs.avail_out == 0) break;              /* 输出满了，下次再来 */
            if (zr == Z_BUF_ERROR) break;              /* 需要更多输入 */
        }
    }

    inflateEnd(&zs);
    out[total] = 0;
    *outlen = total;
    if (!done) {
        mlog("gz incomplete: %d bytes", total);
        return -2;
    }

    /* HTTP/1.1 chunked 的结束块还在路上：把剩下的读干净，socket 才能复用 */
    if (hp->chunked || hp->remaining != 0) {
        char junk[512];
        int guard = 0;

        while (guard++ < 64) {
            int n = http_read(hp, junk, (int)sizeof(junk), 0, NULL, -1, err, errlen);

            if (n > 0) continue;
            if (n == 0) break;
            if (n == -1) { sceKernelDelayThread(2 * 1000); if (guard > 8) break; continue; }
            hp->reused_broken = 1;
            break;
        }
    } else {
        /* Content-Length 型：压缩正文已经读完，标记完成，连接可以复用 */
        hp->finished = 1;
    }
    return 0;
}

int http_get(const char *url, char *out, int outmax, int *outlen,
             int timeout_ms, char *err, int errlen)
{
    Http *hp = &g_get_http;
    int total = 0;
    int r;
    unsigned int t0 = sceKernelGetSystemTimeLow();
    int idle = 0, zeros = 0, minus1 = 0;

    if (outlen) *outlen = 0;
    g_last_expect = -1;

    if (g_get_lock < 0) g_get_lock = sceKernelCreateSema("melody_get", 0, 1, 1, NULL);
    if (g_get_lock >= 0) sceKernelWaitSema(g_get_lock, 1, NULL);

    memset(hp, 0, sizeof(*hp));
    hp->sock = -1;

    g_want_gzip = 1;
    g_want_keepalive = 1;
    r = http_open(hp, url, 0, timeout_ms, err, errlen);
    g_want_gzip = 0;
    g_want_keepalive = 0;
    if (r < 0) {
        if (g_get_lock >= 0) sceKernelSignalSema(g_get_lock, 1);
        return -1;
    }

    if (hp->gzip) {
        int rc = http_inflate(hp, out, outmax, &total, timeout_ms, err, errlen);

        if (rc < 0) hp->reused_broken = 1;
        http_close(hp);
        if (g_get_lock >= 0) sceKernelSignalSema(g_get_lock, 1);
        if (rc < 0) return rc;
        mlog("gz %d bytes", total);
        if (outlen) *outlen = total;
        g_last_expect = -1;
        return 0;
    }

    for (;;) {
        int n;

        if (total >= outmax - 1) break;
        if (aborted()) {
            hp->reused_broken = 1;
            http_close(hp);
            if (g_get_lock >= 0) sceKernelSignalSema(g_get_lock, 1);
            mlog("get aborted by play request");
            return -1;
        }

        n = http_read(hp, out + total, outmax - 1 - total, 0, NULL, -1, err, errlen);
        if (n > 0) { total += n; idle = 0; continue; }
        if (n == 0) { zeros++; break; }
        if (n == -1) {
            minus1++;
            if ((int)((sceKernelGetSystemTimeLow() - t0) / 1000) > timeout_ms) break;
            if (++idle > 3000) break;
            sceKernelDelayThread(3 * 1000);
            continue;
        }
        mlog("get aborted: total=%d n=%d body=%lld remaining=%lld", total, n,
             hp->body_bytes, hp->remaining);
        hp->reused_broken = 1;
        http_close(hp);
        if (g_get_lock >= 0) sceKernelSignalSema(g_get_lock, 1);
        return -2;
    }

    if (hp->content_length > 0 && total < hp->content_length - 16) {
        mlog("get short: total=%d cl=%lld body=%lld remaining=%lld zeros=%d retry=%d eofs=%d",
             total, hp->content_length, hp->body_bytes, hp->remaining, zeros, minus1,
             hp->eof_retry);
        /* 正文没读完的连接不能复用（剩下的字节会把下一个响应冲掉） */
        hp->reused_broken = 1;
    }

    r = (int)hp->content_length;
    g_last_expect = r;
    http_close(hp);
    if (g_get_lock >= 0) sceKernelSignalSema(g_get_lock, 1);

    out[total] = 0;
    if (outlen) *outlen = total;
    (void)r;
    return 0;
}

int http_post_form(const char*url,const char*body,char*out,int outmax,int*outlen,int timeout_ms,char*err,int errlen){
    if(!url||strncmp(url,"http://",7)||!body||strlen(body)>2048||!out||outmax<2||!outlen)return -1;
    *outlen=0;if(g_get_lock<0)g_get_lock=sceKernelCreateSema("melody_get",0,1,1,NULL);
    if(g_get_lock<0||sceKernelWaitSema(g_get_lock,1,NULL)<0)return -1;
    Http*h=&g_get_http;memset(h,0,sizeof(*h));h->sock=-1;h->no_reuse=1;char location[900]={0};int result=-1,total=0;
    post_owner=sceKernelGetThreadId();post_body=body;
    int opened=http_connect_once(h,url,0,-1,timeout_ms,100,err,errlen,location,sizeof(location));
    post_body=NULL;post_owner=-1;
    if(opened<0)goto done;
    /* Mutations are never silently redirected or retried. */
    if(h->status!=200){snprintf(err,(size_t)errlen,"HTTP %d",h->status);goto done;}
    unsigned begin=sceKernelGetSystemTimeLow();
    while(total<outmax-1){int n=http_read(h,out+total,outmax-1-total,100,NULL,-1,err,errlen);
        if(n>0){total+=n;continue;}if(!n){result=0;break;}if(n!=-1)break;
        if(aborted()||(unsigned)(sceKernelGetSystemTimeLow()-begin)>(unsigned)timeout_ms*1000u)break;
    }
    out[total]=0;*outlen=total;
 done:h->reused_broken=1;http_close(h);sceKernelSignalSema(g_get_lock,1);return result;
}

/* 2026-11-01：api 线程被强杀后，net 的全局锁可能还停在"已持有"状态
 * （g_get_lock 包住整个 http_get，任何一条提前 return 都会漏 signal）。
 * 这里把两把锁删掉重建（懒创建），否则 MV 回来后第一个网络请求会永久
 * 阻塞在 sceKernelWaitSema(..., NULL) 上——真机现象就是"播完 MV 网络死"。 */
void net_reset_locks(void)
{
    post_body=NULL;post_owner=-1;
    /* The API worker is already terminated. Its static request survives the
     * thread: close it before the next http_get memset loses the descriptor.
     * Never pool an abandoned response, even if its final read completed. */
    if (g_connect_http == &g_get_http) g_connect_http = NULL;
    g_get_http.reused_broken = 1;
    http_close(&g_get_http);
    g_want_gzip = 0;
    g_want_keepalive = 0;
    if (g_get_lock >= 0) {
        sceKernelDeleteSema(g_get_lock);
        g_get_lock = -1;
    }
    if (g_conn_lock >= 0) {
        sceKernelDeleteSema(g_conn_lock);
        g_conn_lock = -1;
    }
}
