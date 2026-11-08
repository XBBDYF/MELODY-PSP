/*
 * MELODY PSP — 主题自带字体（实现）
 *
 * 只做三件事：解析 skin.json 的 fonts[]、校验 PSPF 字库、把字库挂到 gfx。
 * 绘制逻辑不在这里——gfx.c 的 pick_glyph() 先查主题字体再回退内置。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MELODY_HOST_TEST
#include <pspiofilemgr.h>

#include "app.h"
#include "log.h"
#include "gfx.h"
#endif
#include "skin_font.h"

#define FONT_ARENA_BUDGET (1536 * 1024)   /* 1.5MB，够 2 个字号的中文子集 */
#define FONT_FILE_MAX     (896 * 1024)    /* 单个字库上限 */
#define FONT_DECL_MAX     4

#ifndef MELODY_HOST_TEST
static unsigned char *g_arena;
static int g_arena_used;
static char g_err[160];
static int g_loaded_sizes;
#endif

static const int k_valid_sizes[4] = { 12, 16, 22, 32 };

/* ------------------------------------------------------------------ */
/* 纯函数：解析与校验（宿主机测试直接调）                              */
/* ------------------------------------------------------------------ */

static const char *find_key(const char *json, const char *key, const char *end)
{
    int kl = (int)strlen(key);
    const char *p = json;

    while (p + kl + 2 < end) {
        if (*p == '"' && !strncmp(p + 1, key, (size_t)kl) && p[1 + kl] == '"') {
            p += kl + 2;
            while (p < end && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            return p;
        }
        p++;
    }
    return NULL;
}

int skin_font_parse(const char *json, SkinFontDecl *out, int max)
{
    const char *end, *p;
    int n = 0;

    if (!json || !out || max <= 0) return -1;
    end = json + strlen(json);
    p = find_key(json, "fonts", end);
    if (!p || *p != '[') return 0;
    p++;
    while (p < end && *p && n < max) {
        const char *obj_end;
        const char *size_p, *file_p;
        SkinFontDecl *d = &out[n];
        int ok = 0;

        while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
        if (*p == ']') break;
        if (*p != '{') return -1;
        obj_end = strchr(p, '}');
        if (!obj_end) return -1;

        memset(d, 0, sizeof(*d));
        size_p = find_key(p, "size", obj_end);
        if (size_p && size_p < obj_end && *size_p >= '0' && *size_p <= '9') {
            d->size = atoi(size_p);
            for (int i = 0; i < 4; ++i)
                if (d->size == k_valid_sizes[i]) ok = 1;
        }
        file_p = find_key(p, "file", obj_end);
        if (file_p && file_p < obj_end && *file_p == '"') {
            const char *s = file_p + 1;
            const char *q = strchr(s, '"');
            if (!q || q > obj_end || q - s >= (int)sizeof(d->file)) return -1;
            memcpy(d->file, s, (size_t)(q - s));
            d->file[q - s] = 0;
        }
        if (!ok || !d->file[0]) return -1;
        n++;
        p = obj_end + 1;
    }
    return n;
}

int skin_font_validate(const unsigned char *blob, int len, char *err, int errlen)
{
    unsigned int magic, version, count, rec, off;
    unsigned int prev = 0;

    if (!blob || len < 20) {
        snprintf(err, (size_t)errlen, "字库太短");
        return -1;
    }
    memcpy(&magic, blob + 0, 4);
    memcpy(&version, blob + 4, 4);
    memcpy(&count, blob + 8, 4);
    memcpy(&rec, blob + 12, 4);
    memcpy(&off, blob + 16, 4);

    if (magic != 0x46505350u) {
        snprintf(err, (size_t)errlen, "不是 PSPF 字库");
        return -1;
    }
    if (version != 1) {
        snprintf(err, (size_t)errlen, "字库版本 %u 不支持", version);
        return -1;
    }
    if (rec < 12 || rec > 4096) {
        snprintf(err, (size_t)errlen, "记录长度 %u 非法", rec);
        return -1;
    }
    if (count == 0 || count > 22000) {
        snprintf(err, (size_t)errlen, "字数 %u 非法", count);
        return -1;
    }
    if (off > (unsigned int)len || count > ((unsigned int)len - off) / rec) {
        snprintf(err, (size_t)errlen, "字库越界");
        return -1;
    }
    for (unsigned int i = 0; i < count; ++i) {
        unsigned int cp;

        memcpy(&cp, blob + off + i * rec, 4);
        if (i && cp <= prev) {
            snprintf(err, (size_t)errlen, "码点未升序（第 %u 条）", i);
            return -1;
        }
        prev = cp;
    }
    return 0;
}

#ifndef MELODY_HOST_TEST
/* ------------------------------------------------------------------ */
/* 加载                                                                */
/* ------------------------------------------------------------------ */

static const char *size_name(int size)
{
    switch (size) {
        case 12: return "font12";
        case 16: return "font16";
        case 22: return "font22";
        default: return "font32";
    }
}

static int size_to_fs(int size)
{
    switch (size) {
        case 12: return FS_SM;
        case 16: return FS_MD;
        case 22: return FS_LG;
        default: return FS_XL;
    }
}

static int read_text(const char *path, char *out, int cap)
{
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    int got = 0;

    if (fd < 0) return -1;
    while (got < cap - 1) {
        int n = sceIoRead(fd, out + got, cap - 1 - got);

        if (n <= 0) break;
        got += n;
    }
    sceIoClose(fd);
    out[got] = 0;
    return got;
}

static int read_font_file(const char *path, unsigned char **out, int *out_len)
{
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    SceOff size;
    unsigned char *buf;
    int got = 0;

    if (fd < 0) {
        snprintf(g_err, sizeof(g_err), "打不开 %s", path);
        return -1;
    }
    size = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (size <= 0 || size > FONT_FILE_MAX) {
        sceIoClose(fd);
        snprintf(g_err, sizeof(g_err), "字库大小非法 %d", (int)size);
        return -1;
    }
    if (g_arena_used + (int)size > FONT_ARENA_BUDGET) {
        sceIoClose(fd);
        snprintf(g_err, sizeof(g_err), "字库超出预算（已用 %dKB）", g_arena_used / 1024);
        return -1;
    }
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    buf = g_arena + g_arena_used;
    while (got < (int)size) {
        int n = sceIoRead(fd, buf + got, (int)size - got);

        if (n <= 0) break;
        got += n;
    }
    sceIoClose(fd);
    if (got != (int)size) {
        snprintf(g_err, sizeof(g_err), "读字库不完整 %d/%d", got, (int)size);
        return -1;
    }
    g_arena_used += (int)size;
    *out = buf;
    *out_len = (int)size;
    return 0;
}

int skin_font_apply(const char *root)
{
    char json_path[416];
    char json[8192];
    SkinFontDecl decl[FONT_DECL_MAX];
    int n, applied = 0;

    skin_font_release();
    g_err[0] = 0;

    snprintf(json_path, sizeof(json_path), "%s/skin.json", root);
    if (read_text(json_path, json, (int)sizeof(json)) <= 0) return 0;

    n = skin_font_parse(json, decl, FONT_DECL_MAX);
    if (n < 0) {
        snprintf(g_err, sizeof(g_err), "skin.json 的 fonts 段格式错误");
        mlog("skin font: parse failed");
        return 0;
    }
    if (n == 0) return 0;

    g_arena = malloc(FONT_ARENA_BUDGET);
    if (!g_arena) {
        snprintf(g_err, sizeof(g_err), "字体内存不足");
        return 0;
    }
    g_arena_used = 0;

    for (int i = 0; i < n; ++i) {
        char path[512];
        unsigned char *blob = NULL;
        int len = 0, fs;
        char verr[96];

        snprintf(path, sizeof(path), "%s/%s", root, decl[i].file);
        if (read_font_file(path, &blob, &len) < 0) {
            mlog("skin font: %s failed (%s)", decl[i].file, g_err);
            break;
        }
        if (skin_font_validate(blob, len, verr, (int)sizeof(verr)) < 0) {
            snprintf(g_err, sizeof(g_err), "%s：%s", decl[i].file, verr);
            mlog("skin font: %s invalid (%s)", decl[i].file, verr);
            break;
        }
        fs = size_to_fs(decl[i].size);
        if (gfx_theme_font_attach(fs, blob, blob + len) < 0) {
            snprintf(g_err, sizeof(g_err), "%s：挂载失败", decl[i].file);
            break;
        }
        mlog("skin font: %s -> %s %dKB", decl[i].file, size_name(decl[i].size), len / 1024);
        applied++;
    }

    if (applied != n) {
        /* 任何一项失败：整体回到内置，绝不半套 */
        gfx_theme_font_clear();
        free(g_arena);
        g_arena = NULL;
        g_arena_used = 0;
        g_loaded_sizes = 0;
        mlog("skin font: rolled back (%s)", g_err);
        return 0;
    }
    g_loaded_sizes = applied;
    return applied;
}

void skin_font_release(void)
{
    gfx_theme_font_clear();
    if (g_arena) {
        free(g_arena);
        g_arena = NULL;
    }
    g_arena_used = 0;
    g_loaded_sizes = 0;
}

int skin_font_bytes(void)
{
    return g_arena_used;
}

const char *skin_font_last_error(void)
{
    return g_err;
}
#endif /* MELODY_HOST_TEST */
