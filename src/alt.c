/*
 * MELODY PSP — 备用音源（免费资源回退）
 * ------------------------------------------------------------------
 * 网易官方接口对 VIP 歌只给 45 秒试听，或者干脆不给地址。这里按顺序试别的
 * 公开音源，拿到一个「完整、能播」的 http mp3 直链就停：
 *
 *   1. 局域网网关  自己的 NCM API / UnblockNeteaseMusic 反代，VIP 歌也认
 *   2. 酷我        纯 HTTP、免鉴权、独立曲库
 *   3. GDStudio    自身只有 HTTPS，必须挂在网关的 /gd/ 后面
 *
 * 两个坑（都实测过）：
 *   - 酷我 search.kuwo.cn/r.s 的 "json" 其实是单引号伪 JSON，json_walk 解析
 *     不了，只能当纯文本扫 'MUSICRID':'MUSIC_<数字>' 和 DURATION。
 *   - 酷我对一部分歌只给 30 秒左右的试听碎片，链接下得动、播到一半就断。
 *     所以换到直链后必须再开一次连接看 Content-Length：128kbps 下应该是
 *     时长 × 16000 字节，差太多就丢掉换下一个候选。
 *
 * 内存：所有缓冲都是静态的（约 70KB），既不放线程栈（PSP 工作线程栈才 64KB），
 * 也不借 api.c 的 g_buf（它可能正被别的请求占着）。
 * 只应在 api 工作线程里调用。
 */

#include "alt.h"

#include "json.h"
#include "log.h"
#include "net.h"

#include <pspkernel.h>
#include <pspthreadman.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ALT_FILE   APP_DIR "alt.txt"

#define BODY_MAX   (48 * 1024)   /* 搜索响应 20~40KB */
#define TEXT_MAX   4096          /* 取直链的纯文本响应 ~100 字节 */
#define URL_MAX    1024

#define T_SEARCH   6000
#define T_CONV     5000
#define T_VALID    6000
#define T_TOTAL    12000         /* 整次解析的硬预算：调用方在按播放键，不能久等 */

#define KMAX       10            /* 酷我一次最多看几个候选 */

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

static int  g_en[ALT_SOURCE_COUNT];
static char g_gateway[64];       /* "192.168.1.10:8088"，可带路径前缀 */
static char g_err[160];          /* 给界面看的一句话（多次失败用「；」拼） */

static char g_body[BODY_MAX];    /* 搜索 / 网关 / GDStudio 的响应 */
static char g_text[TEXT_MAX];    /* 酷我换直链的纯文本响应 */
static char g_link[URL_MAX];     /* 酷我当前候选的直链 */

static unsigned int g_t0;        /* 本次 alt_resolve 的起点（us） */
static unsigned int g_gd_us;     /* 上次 GDStudio 请求的起点（限速用） */
static int g_last_size;          /* 上一次成功校验的直链大小（给下载进度用） */

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static int seg_is(const char *p, int len, const char *lit)
{
    int n = (int)strlen(lit);

    return len == n && !strncmp(p, lit, (size_t)n);
}

/* snprintf 截断可能切在多字节字符中间，界面会看到半个汉字 */
static void utf8_fix(char *s)
{
    int n = (int)strlen(s);
    int i = n - 1;

    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    if (i >= 0) {
        unsigned char c = (unsigned char)s[i];
        int need = (c < 0x80) ? 1
                 : ((c & 0xE0) == 0xC0) ? 2
                 : ((c & 0xF0) == 0xE0) ? 3
                 : ((c & 0xF8) == 0xF0) ? 4 : 1;

        if (i + need > n) s[i] = 0;
    }
}

/* 追加一条失败原因（谁失败了、为什么） */
static void alt_err(const char *msg)
{
    int n = (int)strlen(g_err);

    if (n == 0) {
        snprintf(g_err, sizeof(g_err), "%s", msg);
    } else if (n < (int)sizeof(g_err) - 4) {
        snprintf(g_err + n, sizeof(g_err) - (size_t)n, "；%s", msg);
    }
    utf8_fix(g_err);
}

static int budget_ms(void)
{
    unsigned int used = (sceKernelGetSystemTimeLow() - g_t0) / 1000;

    /* 无符号减法会把「超时」变成负数，正好用来判断 */
    return (int)((unsigned int)T_TOTAL - used);
}

/* 把固定的单项超时夹到剩余预算里；返回 0 = 这次别请求了 */
static int tmo(int want)
{
    int left = budget_ms() - 250;

    if (left < 700) return 0;
    return want < left ? want : left;
}

static void set_src(char *src, int srclen, const char *s)
{
    if (src && srclen > 0) snprintf(src, (size_t)srclen, "%s", s);
}

/* "HTTP 404" -> 404 */
static int err_status(const char *err)
{
    if (err && !strncmp(err, "HTTP ", 5)) return atoi(err + 5);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 设置文件                                                            */
/* ------------------------------------------------------------------ */

static int parse_on(const char *v)
{
    if (!v) return 0;
    if (!strcasecmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "true") ||
        !strcasecmp(v, "yes")) return 1;
    return atoi(v) != 0;
}

/* 网关地址：去掉 http:// 前缀和结尾斜杠，允许带路径前缀 */
static void set_host(const char *v)
{
    const char *p = v ? v : "";
    int i = 0;

    if (!strncasecmp(p, "http://", 7)) p += 7;
    else if (!strncasecmp(p, "https://", 8)) p += 8;
    while (*p == ' ' || *p == '\t') p++;

    while (*p && *p != '\r' && *p != '\n' && *p != ' ' && *p != '\t' &&
           i < (int)sizeof(g_gateway) - 1)
        g_gateway[i++] = *p++;
    while (i > 0 && (g_gateway[i - 1] == '/' || g_gateway[i - 1] == ' ')) i--;
    g_gateway[i] = 0;
}

static void set_defaults(void)
{
    memset(g_en, 0, sizeof(g_en));
    g_en[ALT_BYFUNS] = 1;     /* 纯 HTTP 纯文本，默认就开 */
    g_en[ALT_KUWO] = 1;       /* 酷我免配置，默认就开着 */
    g_en[ALT_GATEWAY] = 0;
    g_en[ALT_GDSTUDIO] = 0;
    g_gateway[0] = 0;
}

void alt_init(void)
{
    FILE *fp;
    char line[160];
    int got = 0;

    set_defaults();
    g_err[0] = 0;

    fp = fopen(ALT_FILE, "r");
    if (!fp) {
        mlog("alt: %s 不存在，写默认设置", ALT_FILE);
        alt_save();
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        char *k = line;
        char *v;

        if (!eq) continue;
        *eq = 0;
        v = eq + 1;

        while (*k == ' ' || *k == '\t') k++;
        while (*v == ' ' || *v == '\t') v++;
        {
            int n = (int)strlen(v);
            while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' '))
                v[--n] = 0;
        }
        if (!k[0] || k[0] == '#') continue;

        if (!strcmp(k, "byfuns")) g_en[ALT_BYFUNS] = parse_on(v);
        else if (!strcmp(k, "kuwo")) g_en[ALT_KUWO] = parse_on(v);
        else if (!strcmp(k, "gateway")) g_en[ALT_GATEWAY] = parse_on(v);
        else if (!strcmp(k, "gdstudio")) g_en[ALT_GDSTUDIO] = parse_on(v);
        else if (!strcmp(k, "gateway_host")) set_host(v);
        else continue;
        got = 1;
    }
    fclose(fp);

    if (!got) alt_save();       /* 空文件/全是垃圾行，重写成默认 */

    mlog("alt: byfuns=%d kuwo=%d gateway=%d gd=%d host=[%s]",
         g_en[ALT_BYFUNS], g_en[ALT_KUWO], g_en[ALT_GATEWAY],
         g_en[ALT_GDSTUDIO], g_gateway);
}

void alt_save(void)
{
    FILE *fp = fopen(ALT_FILE, "w");

    if (!fp) {
        mlog("alt: 写 %s 失败", ALT_FILE);
        return;
    }
    fprintf(fp, "byfuns=%d\n", g_en[ALT_BYFUNS] ? 1 : 0);
    fprintf(fp, "kuwo=%d\n", g_en[ALT_KUWO] ? 1 : 0);
    fprintf(fp, "gateway=%d\n", g_en[ALT_GATEWAY] ? 1 : 0);
    fprintf(fp, "gdstudio=%d\n", g_en[ALT_GDSTUDIO] ? 1 : 0);
    fprintf(fp, "gateway_host=%s\n", g_gateway);
    fclose(fp);
    mlog("alt: 设置已保存 (%d/%d/%d/%d)", g_en[ALT_BYFUNS], g_en[ALT_KUWO],
         g_en[ALT_GATEWAY], g_en[ALT_GDSTUDIO]);
}

const char *alt_name(int i)
{
    static const char *n[ALT_SOURCE_COUNT] = { "byfuns", "酷我", "本地网关", "GDStudio" };

    return (i >= 0 && i < ALT_SOURCE_COUNT) ? n[i] : "";
}

const char *alt_desc(int i)
{
    static const char *d[ALT_SOURCE_COUNT] = {
        "纯 HTTP 纯文本，直接回整首 mp3 直链",
        "免费曲库，纯 HTTP 免登录；部分歌只有试听",
        "局域网反代 NCM / UnblockNeteaseMusic，能放 VIP",
        "需要网关代理 /gd/，请求间隔 1.2 秒"
    };

    return (i >= 0 && i < ALT_SOURCE_COUNT) ? d[i] : "";
}

int alt_enabled(int i)
{
    return (i >= 0 && i < ALT_SOURCE_COUNT) ? g_en[i] : 0;
}

void alt_set_enabled(int i, int on)
{
    if (i >= 0 && i < ALT_SOURCE_COUNT) g_en[i] = on ? 1 : 0;
}

const char *alt_gateway(void) { return g_gateway; }

void alt_set_gateway(const char *hp)
{
    set_host(hp);
}

const char *alt_last_error(void) { return g_err; }

int alt_last_size(void) { return g_last_size; }

/* ------------------------------------------------------------------ */
/* 搜索词的清洗                                                        */
/* ------------------------------------------------------------------ */

/* 切掉名字里跟「是哪首歌」无关的尾巴：
 *   (Live) （伴奏） [Remix] 【】 （） feat. XXX / ft. XXX
 * 按 UTF-8 字节序列认全角括号，别用 char 直接比。 */
static void clean_name(const char *in, char *out, int outlen)
{
    int i = 0;
    int k;

    if (!in) in = "";
    if (outlen <= 1) return;

    for (k = 0; in[k] && i < outlen - 1; ) {
        unsigned char c = (unsigned char)in[k];
        unsigned char c1 = (unsigned char)in[k + 1];
        unsigned char c2 = c1 ? (unsigned char)in[k + 2] : 0;   /* 别读到 NUL 后面 */

        if (c == '(' || c == '[' || c == '{') break;
        if (c == 0xEF && c1 == 0xBC && c2 == 0x88) break;              /* （ */
        if (c == 0xE3 && c1 == 0x80 && (c2 == 0x90 || c2 == 0x94)) break; /* 【 〔 */

        /* " feat." / " ft." / " Feat " */
        if (c == ' ' && (!strncasecmp(in + k + 1, "feat", 4) ||
                         !strncasecmp(in + k + 1, "ft.", 3)))
            break;

        out[i++] = (char)c;
        k++;
    }
    out[i] = 0;

    while (i > 0) {
        char c = out[i - 1];

        if (c == ' ' || c == '\t' || c == '-' || c == '_' || c == '&' ||
            c == '/' || c == ',' || c == '.' || c == '|')
            i--;
        else
            break;
    }
    out[i] = 0;

    if (i == 0) snprintf(out, (size_t)outlen, "%s", in);   /* 整个名字都在括号里，退回原名 */
}

/* 网易的多歌手是 "A/B" 拼起来的，酷我按第一个搜就够了 */
static void first_artist(const Song *song, char *out, int outlen)
{
    const char *p = song ? song->artist : "";
    int i = 0;

    if (outlen <= 0) return;
    while (*p == ' ') p++;
    while (*p && *p != '/' && i < outlen - 1) out[i++] = *p++;
    while (i > 0 && out[i - 1] == ' ') i--;
    out[i] = 0;
}

static void build_query(const Song *song, const char *name, char *out, int outlen)
{
    char artist[80];

    first_artist(song, artist, sizeof(artist));
    if (artist[0]) snprintf(out, (size_t)outlen, "%s %s", name, artist);
    else snprintf(out, (size_t)outlen, "%s", name);
}

/* ------------------------------------------------------------------ */
/* 直链清洗                                                            */
/* ------------------------------------------------------------------ */

static int host_suffix(const char *h, int len, const char *suf)
{
    int sl = (int)strlen(suf);

    return len >= sl && !strncasecmp(h + len - sl, suf, sl);
}

/*
 * 只允许 http 直链。https 一律不接（app 还没有 TLS）。
 * 例外：酷我的 CDN 和网易的 music.126.net 都同时监听明文 80 端口，
 * 接口偶尔回 https，把 scheme 换掉就能直接下（2026-10-31 实测 206/audio-mpeg）。
 */
static int host_http_ok(const char *h, int len)
{
    return host_suffix(h, len, ".kuwo.cn") ||
           (len == 7 && !strncasecmp(h, "kuwo.cn", 7)) ||
           host_suffix(h, len, ".music.126.net");
}

static int httpify(const char *in, char *out, int outlen)
{
    const char *u = in;
    const char *host;
    int n, hl;

    while (*u == ' ' || *u == '\t' || *u == '\r' || *u == '\n' ||
           *u == '"' || *u == '\'')
        u++;
    n = (int)strlen(u);
    while (n > 0 && (u[n - 1] == '\r' || u[n - 1] == '\n' || u[n - 1] == ' ' ||
                     u[n - 1] == '\t' || u[n - 1] == '"' || u[n - 1] == '\''))
        n--;
    if (n <= 0 || n >= outlen - 1) return 0;

    if (n > 7 && !strncasecmp(u, "http://", 7)) {
        memcpy(out, u, (size_t)n);
        out[n] = 0;
        return 1;
    }
    if (n > 8 && !strncasecmp(u, "https://", 8)) {
        host = u + 8;
        hl = 0;
        while (host + hl < u + n && host[hl] != '/' && host[hl] != ':' &&
               host[hl] != '?')
            hl++;
        if (!host_http_ok(host, hl)) return 0;
        snprintf(out, (size_t)outlen, "http://%.*s", n - 8, u + 8);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 长度校验：这是「试听碎片」唯一的照妖镜                              */
/* ------------------------------------------------------------------ */

/*
 * 打开直链只读响应头，用 Content-Length 判断是不是整首歌。
 * 128kbps mp3 ≈ 16000 字节/秒，实测酷我的 DURATION 和真实大小差 0.1%，
 * 所以 ±35% 的窗口足够宽，而 30 秒碎片（约 45 万字节）一定掉出去。
 * ref_s <= 0：没有参考时长，只要求 > 400000 字节（约 25 秒）。
 */
static int probe_ok(const char *url, int ref_s)
{
    static Http h;                 /* 17KB，不能放线程栈 */
    char err[64];
    char head[4];
    int tm, size = -1, got = 0, ok = 1;

    tm = tmo(T_VALID);
    if (tm <= 0) {
        alt_err("校验超时");
        return 0;
    }

    /* 不带 Range 的 GET：服务端回 200 + 完整 Content-Length，拿到头就断 */
    if (http_open_range(&h, url, 0, -1, tm, err, sizeof(err)) < 0) {
        mlog("alt: probe open fail (%s) %s", err, url);
        alt_err("直链打不开");
        return 0;
    }
    size = (int)h.content_length;
    if (h.status == 200 || h.status == 206) {
        int r = http_read(&h, head, 3, 1500, NULL, -1, err, sizeof(err));

        got = r > 0 ? r : 0;
    }
    http_close(&h);

    if (size > 0) {
        if (ref_s >= 30) {
            long long expect = (long long)ref_s * 16000;

            if ((long long)size < expect * 65 / 100) {
                alt_err("只有试听片段");
                ok = 0;
            } else if ((long long)size > expect * 135 / 100) {
                alt_err("时长对不上");
                ok = 0;
            }
        } else if (size < 400000) {
            alt_err("文件太小");
            ok = 0;
        }
    }

    /* 直链被换成错误页时第一位是 '<'，这种一定放不出来 */
    if (ok && got >= 1 && (head[0] == '<' || head[0] == '{')) {
        alt_err("直链不是音频");
        ok = 0;
    }

    mlog("alt: probe %d bytes (ref %ds) %s", size, ref_s, ok ? "ok" : "reject");
    if (ok && size > 0) g_last_size = size;
    return ok;
}

/* ------------------------------------------------------------------ */
/* byfuns：纯 HTTP，GET 回来的正文就是一行 mp3 直链（171 字节）          */
/* ------------------------------------------------------------------ */

#define BYFUNS_HOST "api.byfuns.top"

static int byfuns_try(const Song *song, char *out, int outlen)
{
    char url[200];
    char err[64];
    int len = 0, tm;

    tm = tmo(T_CONV);
    if (tm <= 0) {
        alt_err("byfuns 超时");
        return 0;
    }

    snprintf(url, sizeof(url), "http://" BYFUNS_HOST "/1/?id=%lld&level=standard", song->id);
    if (http_get(url, g_text, (int)sizeof(g_text), &len, tm, err, sizeof(err)) < 0 ||
        len <= 0) {
        mlog("alt: byfuns %s -> %s", url, err);
        alt_err("byfuns 连不上");
        return 0;
    }
    g_text[len < (int)sizeof(g_text) ? len : (int)sizeof(g_text) - 1] = 0;

    if (!httpify(g_text, g_link, sizeof(g_link))) {
        mlog("alt: byfuns 返回不是直链: %.60s", g_text);
        alt_err("byfuns 没给直链");
        return 0;
    }
    if (!probe_ok(g_link, song->dur_ms / 1000)) return 0;

    snprintf(out, (size_t)outlen, "%s", g_link);
    mlog("alt: byfuns ok len=%d", len);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 酷我                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int rid;      /* MUSIC_<rid> */
    int dur;      /* DURATION（秒），跟真实文件大小几乎一致 */
    int score;    /* 排序用：越小越优先 */
} KCand;

typedef struct {
    char url[900];
    int size;
    int code;
    int trial;
    int found;
} LinkCtx;

/* 在 [b,e) 里找 'KEY':<数字> —— 伪 JSON 的键是带单引号的 */
static int field_int(const char *b, const char *e, const char *key)
{
    int kl = (int)strlen(key);
    const char *p = b;

    while (p + kl < e) {
        if (!strncmp(p, key, (size_t)kl)) {
            const char *q = p + kl;

            while (q < e && (*q == '\'' || *q == ':' || *q == ' ')) q++;
            if (q < e && *q >= '0' && *q <= '9') return atoi(q);
        }
        p++;
    }
    return 0;
}

/*
 * 扫 'MUSICRID':'MUSIC_<rid>'，再回到本条的 '{' 里取 DURATION。
 * 字段顺序是字母序，DURATION 在 MUSICRID 前面，所以不能简单往后找。
 */
#define KW_RID     "'MUSICRID':'MUSIC_"
#define KW_RID_LEN ((int)sizeof(KW_RID) - 1)

static int parse_cands(const char *s, KCand *out, int max)
{
    const char *p = s;
    int n = 0;

    while (n < max) {
        const char *hit = strstr(p, KW_RID);
        const char *d, *b, *e;
        long long rid = 0;

        if (!hit) break;
        d = hit + KW_RID_LEN;
        while (*d >= '0' && *d <= '9') {
            if (rid < 2000000000LL) rid = rid * 10 + (*d - '0');
            d++;
        }
        if (rid <= 0) {
            p = hit + KW_RID_LEN;
            continue;
        }

        b = hit;
        while (b > s && b[-1] != '{' && b[-1] != '[') b--;
        for (e = d; *e && *e != '}'; e++) ;

        out[n].rid = (int)rid;
        out[n].dur = field_int(b, e, "DURATION");
        out[n].score = 0;
        n++;
        p = d;
    }
    return n;
}

/* 时长最接近网易那首的排最前；没有参考时长就挑最长的（碎片一般都很短） */
static void rank_cands(KCand *c, int n, int song_dur)
{
    int i, j;

    for (i = 0; i < n; i++) {
        if (song_dur > 0) {
            int d = c[i].dur - song_dur;

            if (d < 0) d = -d;
            c[i].score = c[i].dur > 0 ? d : 100000;
        } else {
            c[i].score = -c[i].dur;
        }
    }

    for (i = 1; i < n; i++) {          /* 插入排序，n ≤ 10 */
        KCand t = c[i];

        for (j = i - 1; j >= 0 && c[j].score > t.score; j--) c[j + 1] = c[j];
        c[j + 1] = t;
    }
}

/* 搜索：1 = 有候选，0 = 真没结果，-1 = 请求本身失败（别拿歌手名再试一遍） */
static int kuwo_search(const char *q, KCand *cand, int *n)
{
    char enc[600];
    char url[760];
    char err[72];
    int len = 0, tm;

    *n = 0;
    net_url_encode(q, enc, sizeof(enc));
    snprintf(url, sizeof(url),
             "http://search.kuwo.cn/r.s?all=%s&ft=music&itemset=web_2013"
             "&client=kt&pn=0&rn=8&rformat=json&encoding=utf8",
             enc);

    tm = tmo(T_SEARCH);
    if (tm <= 0) return -1;
    if (http_get(url, g_body, sizeof(g_body), &len, tm, err, sizeof(err)) < 0 ||
        len <= 0) {
        mlog("alt: kuwo search fail (%s)", err);
        return -1;
    }

    *n = parse_cands(g_body, cand, KMAX);
    mlog("alt: kuwo search %d cand len=%d", *n, len);
    return *n > 0;
}

/* anti.s 回的是纯文本直链，不是 JSON */
static int kuwo_link(int rid)
{
    char url[192];
    char err[72];
    int len = 0, tm;

    tm = tmo(T_CONV);
    if (tm <= 0) {
        alt_err("取直链超时");
        return 0;
    }
    snprintf(url, sizeof(url),
             "http://antiserver.kuwo.cn/anti.s?type=convert_url&format=mp3"
             "&response=url&rid=MUSIC_%d",
             rid);
    if (http_get(url, g_text, sizeof(g_text), &len, tm, err, sizeof(err)) < 0 ||
        len <= 0) {
        mlog("alt: kuwo anti fail rid=%d (%s)", rid, err);
        alt_err("酷我取直链失败");
        return 0;
    }
    if (!httpify(g_text, g_link, sizeof(g_link))) {
        mlog("alt: kuwo 直链不可用: %.64s", g_text);
        alt_err("酷我直链不可用");
        return 0;
    }
    return 1;
}

/* ---- 酷我搜索作为「第三方搜索结果源」 --------------------------------
 *
 * 网易的公开搜索索引对热门华语歌手经常先给翻唱、Live、他人作品（现场实测：
 * 搜“周杰伦”前 10 条里没有一首他的正式录音室作品）。酷我搜索是纯 HTTP、
 * 免鉴权，返回的正是原唱条目，所以把它作为补充来源：结果进列表、标「酷我」，
 * 播放/下载仍走备用音源（playable=2 的既有路径）。
 * 这里只做一次性抓取 + 纯文本解析，不发第二次请求。 */

static void kw_unescape(char *s)
{
    static const struct { const char *ent; char ch; } map[] = {
        { "&nbsp;", ' ' }, { "&amp;", '&' }, { "&#39;", '\'' }, { "&apos;", '\'' },
        { "&quot;", '"' }, { "&lt;", '<' }, { "&gt;", '>' },
        /* Kuwo escapes some characters as JSON-ish sequences inside its pseudo JSON. */
        { "\\\\u0026", '&' }, { "\\u0026", '&' }, { "\\\\u0027", '\'' }, { "\\u0027", '\'' }
    };
    char *r = s;

    while (*s) {
        int hit = 0;
        if (*s == '&' || *s == '\\') {
            for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
                size_t n = strlen(map[i].ent);
                if (!strncmp(s, map[i].ent, n)) { *r++ = map[i].ch; s += n; hit = 1; break; }
            }
        }
        if (!hit) *r++ = *s++;
    }
    *r = 0;
}

/* 从 [b,e) 里取 'KEY':'value'，最多 cap-1 个字节，并还原常见 HTML 实体。 */
static int kw_field(const char *b, const char *e, const char *key, char *out, int cap)
{
    int kl = (int)strlen(key);

    if (!out || cap <= 0) return 0;
    out[0] = 0;
    for (const char *p = b; p + kl + 3 < e; ++p) {
        const char *v;
        int n = 0;

        if (p[0] != '\'' || strncmp(p + 1, key, (size_t)kl) ||
            p[1 + kl] != '\'' || p[2 + kl] != ':') continue;
        v = p + 3 + kl;
        if (v >= e || *v != '\'') return 0;
        ++v;
        while (v < e && *v != '\'' && n < cap - 1) out[n++] = *v++;
        out[n] = 0;
        kw_unescape(out);
        return 1;
    }
    return 0;
}

static int parse_search_songs(const char *s, Song *out, int max)
{
    const char *p = s;
    int n = 0;

    while (n < max) {
        const char *hit = strstr(p, KW_RID);
        const char *d, *b, *e;
        long long rid = 0;
        char name[72] = "", artist[64] = "", album[44] = "";

        if (!hit) break;
        d = hit + KW_RID_LEN;
        while (*d >= '0' && *d <= '9') {
            if (rid < 2000000000LL) rid = rid * 10 + (*d - '0');
            d++;
        }
        if (rid <= 0) { p = hit + KW_RID_LEN; continue; }

        b = hit;
        while (b > s && b[-1] != '{' && b[-1] != '[') b--;
        for (e = d; *e && *e != '}'; e++) ;
        if (!*e) break;

        /* SONGNAME 带版本后缀（"花海&nbsp;(DJ版)"），NAME 是干净标题：
         * 显示用 SONGNAME，排序用 NAME 判断是否翻唱/改版。 */
        if (!kw_field(b, e, "SONGNAME", name, sizeof(name)) ||
            !name[0]) kw_field(b, e, "NAME", name, sizeof(name));
        kw_field(b, e, "ARTIST", artist, sizeof(artist));
        kw_field(b, e, "ALBUM", album, sizeof(album));
        if (!name[0] || !artist[0]) { p = d; continue; }

        {
            Song *song = &out[n];
            int dur = field_int(b, e, "DURATION");

            /* 酷我的相关搜索里混着片段、铃声、伴奏、纯音乐和合辑：这些不是能
             * 当歌听的条目，直接丢掉，避免列表被灌满。 */
            if (dur > 0 && (dur < 45 || dur > 600)) { p = d; continue; }
            if (strstr(name, "片段") || strstr(name, "铃声") || strstr(name, "mp3.2") ||
                strstr(name, "伴奏") || strstr(name, "纯音乐") || strstr(name, "KTV") ||
                strstr(name, "试听")) {
                p = d;
                continue;
            }
            memset(song, 0, sizeof(*song));
            song->id = rid;
            snprintf(song->name, sizeof(song->name), "%s", name);
            snprintf(song->artist, sizeof(song->artist), "%s", artist);
            snprintf(song->album, sizeof(song->album), "%s", album);
            song->dur_ms = dur > 0 ? dur * 1000 : 0;
            song->playable = 2;   /* 无网易地址：播放/下载直接走备用音源 */
            song->fee = -1;       /* 标记「外源」，界面上显示来源而不是 VIP */
            ++n;
        }
        p = d;
    }
    return n;
}

/* 0 = 未启用/失败/无结果；>0 = 写入 out 的条数 */
int alt_kuwo_search_songs(const char *query, Song *out, int max)
{
    char enc[400];
    char url[620];
    char err[72];
    int len = 0, tm, n;

    if (!query || !query[0] || !out || max <= 0) return 0;
    if (!g_en[ALT_KUWO]) return 0;
    if (max > KMAX) max = KMAX;
    net_url_encode(query, enc, sizeof(enc));
    snprintf(url, sizeof(url),
             "http://search.kuwo.cn/r.s?all=%s&ft=music&itemset=web_2013"
             "&client=kt&pn=0&rn=%d&rformat=json&encoding=utf8",
             enc, max);
    tm = tmo(T_SEARCH);
    if (tm <= 0) return 0;
    if (http_get(url, g_body, sizeof(g_body), &len, tm, err, sizeof(err)) < 0 || len <= 0) {
        mlog("alt: kuwo list search fail (%s)", err);
        return 0;
    }
    n = parse_search_songs(g_body, out, max);
    mlog("alt: kuwo list search rows=%d len=%d", n, len);
    return n;
}

static int kuwo_try(const Song *song, char *out, int outlen)
{
    char name[80];
    char q[200];
    KCand cand[KMAX];
    int song_dur = song->dur_ms / 1000;
    int n = 0, tries, i, sr;

    if (song_dur < 30 || song_dur > 1800) song_dur = 0;   /* 时长不可信就别用 */

    clean_name(song->name, name, sizeof(name));
    build_query(song, name, q, sizeof(q));
    mlog("alt: kuwo query [%s] dur=%ds", q, song_dur);

    sr = kuwo_search(q, cand, &n);
    if (sr < 0) {
        alt_err("酷我搜索失败");
        return 0;
    }
    if (sr == 0) {
        /* 第一次没结果：用歌手名再搜一次（很多翻唱/合集不写原唱名） */
        char artist[80];

        first_artist(song, artist, sizeof(artist));
        if (!artist[0] || kuwo_search(artist, cand, &n) <= 0) {
            alt_err("酷我无结果");
            return 0;
        }
    }

    rank_cands(cand, n, song_dur);
    tries = n < 2 ? n : 2;

    for (i = 0; i < tries; i++) {
        int ref = song_dur > 0 ? song_dur : cand[i].dur;

        if (budget_ms() < 1500) {
            alt_err("酷我超时");
            break;
        }
        if (!kuwo_link(cand[i].rid)) continue;
        if (!probe_ok(g_link, ref)) continue;

        snprintf(out, (size_t)outlen, "%s", g_link);
        mlog("alt: kuwo ok rid=%d ref=%ds", cand[i].rid, ref);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 局域网网关（NCM API / UnblockNeteaseMusic）                         */
/* ------------------------------------------------------------------ */

static void gw_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    LinkCtx *c = (LinkCtx *)ud;
    const char *seg[8];
    int len[8];
    int n = json_split_path(path, seg, len, 8);

    if (n == 1) {
        if (type == JSON_NUM && seg_is(seg[0], len[0], "code")) c->code = (int)num;
        return;
    }
    if (n < 3 || !seg_is(seg[0], len[0], "data") || atoi(seg[1]) != 0) return;

    if (seg_is(seg[2], len[2], "freeTrialInfo")) {
        /* null = 整首；对象/数字/字符串 = 只有试听 */
        if (type != JSON_NULL) c->trial = 1;
        return;
    }
    if (n != 3) return;
    if (type == JSON_STR && seg_is(seg[2], len[2], "url")) {
        if (str && str[0]) {
            snprintf(c->url, sizeof(c->url), "%s", str);
            c->found = 1;
        }
    } else if (type == JSON_NUM && seg_is(seg[2], len[2], "size")) {
        c->size = (int)num;
    }
}

static int gw_call(const Song *song, const char *prefix, LinkCtx *ctx, int *status)
{
    char url[400];
    char err[72];
    int len = 0, tm;

    memset(ctx, 0, sizeof(*ctx));
    *status = 0;

    tm = tmo(T_CONV);
    if (tm <= 0) return 0;

    snprintf(url, sizeof(url), "http://%s%s?id=%lld&level=standard",
             g_gateway, prefix, song->id);
    if (http_get(url, g_body, sizeof(g_body), &len, tm, err, sizeof(err)) < 0 ||
        len <= 0) {
        *status = err_status(err);
        mlog("alt: gw %s -> %s", url, err);
        return 0;
    }
    json_walk(g_body, len, gw_cb, ctx);
    return ctx->found;
}

static int gateway_try(const Song *song, char *out, int outlen)
{
    static const char *paths[2] = { "/ncm/song/url/v1", "/song/url/v1" };
    LinkCtx c;
    int st = 0, i;

    for (i = 0; i < 2; i++) {
        if (gw_call(song, paths[i], &c, &st)) {
            if (c.code != 0 && c.code != 200) {
                alt_err("网关返回错误码");
                return 0;
            }
            if (c.trial) {
                alt_err("网关只给试听");
                return 0;
            }
            if (!httpify(c.url, out, outlen)) {
                alt_err("网关直链不可用");
                return 0;
            }
            mlog("alt: gw ok via %s (%d bytes)", paths[i], c.size);
            return 1;
        }
        /* 只有「前缀不对」才值得换一个再试 */
        if (st != 404 && st != 400 && st != 405) break;
        mlog("alt: gw %s HTTP %d，换下一个前缀", paths[i], st);
    }

    alt_err(st == 404 ? "网关没有这个接口" : "网关请求失败");
    return 0;
}

/* ------------------------------------------------------------------ */
/* GDStudio（本身只有 HTTPS，必须走网关的 /gd/）                       */
/* ------------------------------------------------------------------ */

static void gd_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    LinkCtx *c = (LinkCtx *)ud;

    if (type == JSON_STR && !strcmp(path, "url")) {
        if (str && str[0]) {
            snprintf(c->url, sizeof(c->url), "%s", str);
            c->found = 1;
        }
    } else if (type == JSON_NUM && !strcmp(path, "size")) {
        c->size = (int)num;
    }
}

/* 官方要求两次请求间隔 ≥1200ms，否则直接 403 */
static void gd_wait(void)
{
    if (g_gd_us) {
        int el = (int)((sceKernelGetSystemTimeLow() - g_gd_us) / 1000);

        if (el < 1300) sceKernelDelayThread((SceUInt)(1300 - el) * 1000);
    }
}

static int gd_try(const Song *song, char *out, int outlen)
{
    char url[400];
    char err[72];
    LinkCtx c;
    int len = 0, tm;

    if (budget_ms() < 3500) {
        alt_err("GDStudio 时间不够");
        return 0;
    }
    gd_wait();

    tm = tmo(T_CONV);
    if (tm <= 0) {
        alt_err("GDStudio 超时");
        return 0;
    }

    snprintf(url, sizeof(url),
             "http://%s/gd/api.php?types=url&source=netease&id=%lld&br=128",
             g_gateway, song->id);
    memset(&c, 0, sizeof(c));
    g_gd_us = sceKernelGetSystemTimeLow();
    if (http_get(url, g_body, sizeof(g_body), &len, tm, err, sizeof(err)) < 0 ||
        len <= 0) {
        mlog("alt: gd %s -> %s", url, err);
        alt_err("GDStudio 请求失败");
        return 0;
    }

    json_walk(g_body, len, gd_cb, &c);
    if (!c.found) {
        alt_err("GDStudio 无直链");
        return 0;
    }
    if (!httpify(c.url, out, outlen)) {
        alt_err("GDStudio 直链不可用");
        return 0;
    }
    mlog("alt: gd ok (%d bytes)", c.size);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 对外入口                                                            */
/* ------------------------------------------------------------------ */

int alt_resolve(const Song *song, char *url, int urllen, char *src, int srclen)
{
    g_t0 = sceKernelGetSystemTimeLow();
    g_err[0] = 0;
    if (url && urllen > 0) url[0] = 0;
    if (src && srclen > 0) src[0] = 0;

    if (!song || !url || urllen <= 0) {
        alt_err("参数错误");
        return 0;
    }

    /* 1. byfuns：纯 HTTP、几十字节响应，最有希望先成功 */
    if (g_en[ALT_BYFUNS]) {
        if (byfuns_try(song, url, urllen)) {
            set_src(src, srclen, "byfuns");
            return 1;
        }
    }

    /* 2. 网关：启用且填了地址才走（空地址会拼出 http:///ncm/...） */
    if (g_en[ALT_GATEWAY] && g_gateway[0]) {
        if (gateway_try(song, url, urllen)) {
            set_src(src, srclen, "网关");
            return 1;
        }
    } else if (g_en[ALT_GATEWAY]) {
        mlog("alt: 网关已启用但没填地址，跳过");
    }

    /* 2. 酷我：免配置，通常是唯一能用的那个 */
    if (g_en[ALT_KUWO]) {
        if (kuwo_try(song, url, urllen)) {
            set_src(src, srclen, "酷我");
            return 1;
        }
    }

    /* 3. GDStudio：同样依赖网关 */
    if (g_en[ALT_GDSTUDIO] && g_gateway[0]) {
        if (gd_try(song, url, urllen)) {
            set_src(src, srclen, "GDStudio");
            return 1;
        }
    } else if (g_en[ALT_GDSTUDIO]) {
        mlog("alt: GDStudio 需要网关，跳过");
    }

    if (!g_err[0]) alt_err("没有启用的备用源");
    url[0] = 0;
    mlog("alt: 全部失败 (%s)", g_err);
    return 0;
}
