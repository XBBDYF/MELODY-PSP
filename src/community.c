/* MELODY: local privacy consent + data-only, bounded announcements.
 * Build host parser tests: gcc -DCOMMUNITY_HOST_TEST src/community.c -o test.exe
 */
#include "community.h"
#include <string.h>
#include <stdio.h>
#ifndef COMMUNITY_HOST_TEST
#include <pspiofilemgr.h>
#include "app.h"
#include "net.h"
#endif

static const char k_privacy_text[] =
    "MELODY PSP 隐私政策\n"
    "请阅读后选择接受；拒绝将退出程序。\n\n"
    "在您接受前，程序不发送任何 HTTP 请求。\n"
    "接受后，联网功能会访问网易云音乐接口、\n"
    "音乐及图片服务，以及配置的更新和公告服务。\n"
    "服务端可接收 IP 地址、请求内容和必要请求头。\n"
    "PSP联网可能使用明文HTTP，传输非端到端加密。\n"
    "搜索词、歌曲或歌单编号随对应操作发送；\n"
    "登录功能会向对应服务发送登录相关信息。\n"
    "登录凭据、收藏、历史、缓存及日志可能\n"
    "保存在记忆棒；请勿把登录文件分享给他人。\n"
    "远程服务各自处理所接收的数据，\n"
    "其隐私规则以对应服务提供的说明为准。\n\n"
    "接受记录和已忽略公告编号仅保存在本机。\n"
    "公告是未经签名的纯文本，不执行代码、\n"
    "链接或远程界面命令；请勿据此提交密码。\n"
    "删除 privacy-accepted.txt 后会再次询问。\n"
    "作者：洗不白de衣服 / GitHub：XBBDYF\n"
    "博客：xbbdyf.github.io\n";

const char *community_privacy_text(void) { return k_privacy_text; }

static int valid_id(const char *s, int n)
{
    int i;
    if (n < 1 || n > COMMUNITY_ANNOUNCEMENT_ID_MAX) return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int community_announcement_parse(const char *data, int length,
                                 const char *ignored_id,
                                 CommunityAnnouncement *out)
{
    const char *a, *b;
    int idn, tn, bn, i;
    if (!data || !out || length < 5 ||
        length > COMMUNITY_ANNOUNCEMENT_TEXT_MAX + 70) return -1;
    if (length >= 3 && (unsigned char)data[0] == 0xef &&
        (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf) {
        data += 3; length -= 3;
    }
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)data[i];
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127)
            return -1;
    }
    a = (const char *)memchr(data, '\n', (size_t)length);
    if (!a) return -1;
    idn = (int)(a - data);
    if (idn && data[idn - 1] == '\r') --idn;
    if (!valid_id(data, idn)) return -1;
    b = (const char *)memchr(a + 1, '\n', (size_t)(length - (a + 1 - data)));
    if (!b) return -1;
    tn = (int)(b - a - 1);
    if (tn && a[tn] == '\r') --tn;
    bn = (int)(data + length - b - 1);
    if (tn < 1 || tn > 256 || bn < 1 ||
        tn + bn > COMMUNITY_ANNOUNCEMENT_TEXT_MAX) return -1;
    if (ignored_id && strlen(ignored_id) == (size_t)idn &&
        memcmp(data, ignored_id, (size_t)idn) == 0) return 1;
    memcpy(out->id, data, (size_t)idn); out->id[idn] = 0;
    memcpy(out->title, a + 1, (size_t)tn); out->title[tn] = 0;
    /* Normalize CRLF, preserving body paragraphs. */
    for (i = 0, bn = 0; b + 1 + i < data + length; ++i) {
        char c = b[1 + i];
        if (c != '\r') out->text[bn++] = c;
    }
    out->text[bn] = 0;
    return 0;
}

#ifndef COMMUNITY_HOST_TEST
#define PRIVACY_FILE APP_DIR "privacy-accepted.txt"
#define IGNORE_FILE APP_DIR "announcement-ignored.txt"
static const char k_consent[] = "MELODY-PRIVACY-1\n";
static volatile int g_state;
static CommunityAnnouncement g_announcement;
static int g_checked,g_manual_check;

/* Read to EOF and reject truncation. -2 is strictly a missing file. */
static int read_local(const char *path, char *buf, int cap)
{
    int fd, n = 0, r;
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return fd == (int)0x80010002 ? -2 : -1;
    do {
        r = sceIoRead(fd, buf + n, (unsigned)(cap - 1 - n));
        if (r > 0) n += r;
    } while (r > 0 && n < cap - 1);
    if (n == cap - 1) {
        char extra;
        r = sceIoRead(fd, &extra, 1);
        if (r != 0) { sceIoClose(fd); return -1; }
    }
    if (sceIoClose(fd) < 0 || r < 0) return -1;
    buf[n] = 0;
    return n;
}

static int write_local(const char *path, const char *text)
{
    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int ok, n = (int)strlen(text);
    if (fd < 0) return -1;
    ok = sceIoWrite(fd, text, (unsigned)n) == n;
    if (sceIoClose(fd) < 0) ok = 0;
    if (sceIoSync("ms0:", 0) < 0) ok = 0;
    return ok ? 0 : -1;
}

int community_privacy_accepted(void)
{
    char b[64];
    int n = read_local(PRIVACY_FILE, b, sizeof(b));
    return n == (int)sizeof(k_consent) - 1 &&
           memcmp(b, k_consent, sizeof(k_consent) - 1) == 0;
}
int community_privacy_accept(void) { return write_local(PRIVACY_FILE, k_consent); }

/* Same first nonempty/noncomment line convention as the update worker. */
static int read_url(const char *path, char *url, int cap)
{
    char b[1200], *p;
    int n = read_local(path, b, sizeof(b));
    if (n < 0) return n;
    if (memchr(b, 0, (size_t)n)) return -1;
    p = b;
    while (*p) {
        char *end = strchr(p, '\n'), *tail;
        if (end) *end = 0;
        while (*p == ' ' || *p == '\t') ++p;
        tail = p + strlen(p);
        while (tail > p && (tail[-1] == '\r' || tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = 0;
        if (*p && *p != '#') {
            if (strlen(p) >= (size_t)cap || strncmp(p, "http://", 7)) return -1;
            strcpy(url, p);
            return 0;
        }
        if (!end) break;
        p = end + 1;
    }
    return -1;
}

static int announcement_url(char *url, int cap)
{
    char *slash, *end;
    int r = read_url(APP_DIR "announcement-url.txt", url, cap);
    /* An explicitly present but invalid override is an error, not fallback. */
    if (r != -2) return r;
    if (read_url(APP_DIR "update.txt", url, cap) < 0) return -1;
    end = strpbrk(url + 7, "?#");
    if (end) *end = 0;
    slash = strrchr(url + 7, '/');
    if (!slash) {
        if (strlen(url) + sizeof("/announcement.txt") > (size_t)cap) return -1;
        strcat(url, "/announcement.txt");
    } else {
        if ((size_t)(slash + 1 - url) + sizeof("announcement.txt") > (size_t)cap)
            return -1;
        strcpy(slash + 1, "announcement.txt");
    }
    return 0;
}

static void publish(int state)
{
    __sync_synchronize();
    g_state = state;
}
int community_announcement_state(void)
{
    int state = g_state;
    __sync_synchronize();
    return state;
}
const CommunityAnnouncement *community_announcement_get(void)
{
    return community_announcement_state() == COMMUNITY_ANNOUNCEMENT_READY
        ? &g_announcement : NULL;
}
int community_announcement_check(void)
{
    char url[1000], buf[COMMUNITY_ANNOUNCEMENT_TEXT_MAX + 72];
    char ignored[COMMUNITY_ANNOUNCEMENT_ID_MAX + 3], err[96];
    int n = 0, r, expected;
    /* No HTTP before consent, even if caller accidentally queues this early. */
    if (!community_privacy_accepted()) return -1;
    if (g_checked) return 0;
    g_checked = 1;
    publish(COMMUNITY_ANNOUNCEMENT_CHECKING);
    if (announcement_url(url, sizeof(url)) < 0) goto fail;
    ignored[0] = 0;
    r = read_local(IGNORE_FILE, ignored, sizeof(ignored));
    if (r < 0) ignored[0] = 0;
    else {
        while (r > 0 && (ignored[r - 1] == '\r' || ignored[r - 1] == '\n'))
            ignored[--r] = 0;
        if (!valid_id(ignored, r)) ignored[0] = 0;
    }
    if (http_get(url, buf, sizeof(buf), &n, 8000, err, sizeof(err)) < 0) goto fail;
    expected = http_get_expect();
    if (n < 0 || n >= (int)sizeof(buf) - 1 || (expected >= 0 && expected != n))
        goto fail;
    r = community_announcement_parse(buf, n, g_manual_check?"":ignored, &g_announcement);
    if (r < 0) goto fail;
    publish(r == 1 ? COMMUNITY_ANNOUNCEMENT_NONE : COMMUNITY_ANNOUNCEMENT_READY);
    return 0;
fail:
    publish(COMMUNITY_ANNOUNCEMENT_ERROR);
    return -1;
}
int community_announcement_manual_check(void){
    if(g_state==COMMUNITY_ANNOUNCEMENT_CHECKING)return -1;
    g_checked=0;g_manual_check=1;publish(COMMUNITY_ANNOUNCEMENT_CHECKING);
    int result=community_announcement_check();g_manual_check=0;return result;
}
int community_announcement_dismiss(int dont_show_again)
{
    if (g_state != COMMUNITY_ANNOUNCEMENT_READY) return 0;
    if (dont_show_again && write_local(IGNORE_FILE, g_announcement.id) < 0)
        return -1;
    publish(COMMUNITY_ANNOUNCEMENT_NONE);
    return 0;
}
#else
#include <assert.h>
int main(void)
{
    CommunityAnnouncement out;
    const char *first = "news-20261106\n公告标题\n第一段\n第二段\n";
    const char *next = "news-20261107\r\n新公告\r\n新的正文\r\n";
    char tooLong[2200], longID[100];
    memset(&out, 0, sizeof(out));
    assert(community_announcement_parse(first, (int)strlen(first), "", &out) == 0);
    assert(strcmp(out.id, "news-20261106") == 0);
    assert(strcmp(out.text, "第一段\n第二段\n") == 0);
    assert(community_announcement_parse(first, (int)strlen(first), "news-20261106", &out) == 1);
    assert(strcmp(out.id, "news-20261106") == 0);
    assert(community_announcement_parse(next, (int)strlen(next), "news-20261106", &out) == 0);
    assert(strcmp(out.id, "news-20261107") == 0);
    assert(strcmp(out.text, "新的正文\n") == 0);
    strcpy(tooLong, "tooLong\nTitle\n");
    memset(tooLong + strlen(tooLong), 'x', 2100);
    assert(community_announcement_parse(tooLong, 2114, "", &out) == -1);
    memset(longID, 'a', 65); strcpy(longID + 65, "\nTitle\nBody");
    assert(community_announcement_parse(longID, (int)strlen(longID), "", &out) == -1);
    assert(community_announcement_parse("id\nTitle\nA\0B", 12, "", &out) == -1);
    assert(community_announcement_parse("id\n\nBody", 8, "", &out) == -1);
    puts("PASS: new ID, ignored duplicate, different new ID, CRLF, tooLong, ID65, NUL, empty title");
    return 0;
}
#endif
