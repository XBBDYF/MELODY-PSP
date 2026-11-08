/*
 * MELODY PSP - 本地音乐（记忆棒里自带的歌）与导出
 * ------------------------------------------------------------------
 * · 扫描 ms0:/MUSIC、ms0:/PSP/MUSIC 里的 .mp3，供「本地」页列出播放
 * · 把缓存里的歌导出到 ms0:/MUSIC/MELODY-<id>.mp3，并写入 ID3v2.3 标签
 *   （文件名用 ASCII，中文标题/歌手放在 ID3 里，PSP 的 XMB 会读 ID3 显示）
 */

#include "local.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "sysinfo.h"
#include "cache.h"

/* 256 * 332 = 84992 bytes (83 KiB), versus 166 KiB for 512 entries.
 * Keep PSP-1000 memory headroom for playback, artwork and the API worker. */
#define LOCAL_MAX      256
#define LOCAL_SCAN_DEPTH 32
#define LOCAL_COPY_BUF 16384

typedef struct {
    char path[256];
    char name[72];
    int size;
} LocalFile;

static LocalFile g_loc[LOCAL_MAX];
static int g_loc_n;
static int g_loc_scanned;

static volatile int g_exp_pct;
static volatile int g_exp_active;
static volatile int g_exp_ok = -1;
static char g_exp_dst[256];

/* ------------------------------------------------------------------ */
/* 扫描                                                                */
/* ------------------------------------------------------------------ */

static int has_mp3_ext(const char *name)
{
    int n = (int)strlen(name);

    if (n < 5) return 0;
    return !strcasecmp(name + n - 4, ".mp3");
}

static void scan_dir(const char *dir)
{
    /* Iterative DFS keeps a single dirent/path buffer, rather than one per
     * recursive call. At most 32 open directories, including the root. */
    struct {
        SceUID fd;
        unsigned int path_len;
    } frames[LOCAL_SCAN_DEPTH];
    SceIoDirent ent;
    char path[sizeof(g_loc[0].path)];
    int depth = 0;
    int melody_done = 0;
    int music_root = !strcasecmp(dir, "ms0:/MUSIC");
    int n;

    if (g_loc_n >= LOCAL_MAX) return;
    n = snprintf(path, sizeof(path), "%s", dir);
    if (n < 0 || n >= (int)sizeof(path)) return;
    frames[0].fd = sceIoDopen(path);
    if (frames[0].fd < 0) return;
    frames[0].path_len = (unsigned int)n;

    /* Visit the exported subtree first, then skip its root directory entry.
     * If it cannot be opened here, allow the normal walk to try that entry. */
    if (music_root) {
        n = snprintf(path, sizeof(path), "%s/MELODY", dir);
        if (n >= 0 && n < (int)sizeof(path)) {
            frames[1].fd = sceIoDopen(path);
            if (frames[1].fd >= 0) {
                frames[1].path_len = (unsigned int)n;
                depth = 1;
                melody_done = 1;
            }
        }
        if (!depth) path[frames[0].path_len] = 0;
    }

    while (depth >= 0 && g_loc_n < LOCAL_MAX) {
        unsigned int parent_len = frames[depth].path_len;
        int r, i;

        memset(&ent, 0, sizeof(ent));
        r = sceIoDread(frames[depth].fd, &ent);
        if (r <= 0) {
            sceIoDclose(frames[depth].fd);
            if (--depth >= 0) path[frames[depth].path_len] = 0;
            continue;
        }
        ent.d_name[sizeof(ent.d_name) - 1] = 0;
        if (!ent.d_name[0] || !strcmp(ent.d_name, ".") ||
            !strcmp(ent.d_name, "..")) continue;
        /* A dirent is one component; reject malformed traversal names. */
        if (strchr(ent.d_name, '/') || strchr(ent.d_name, '\\') ||
            strchr(ent.d_name, ':')) continue;
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            if (music_root && depth == 0 && melody_done &&
                !strcasecmp(ent.d_name, "MELODY")) continue;
            if (depth + 1 >= LOCAL_SCAN_DEPTH) {
                mlog("local: directory depth limit, skip %s/%s", path, ent.d_name);
                continue;
            }
        } else if (!FIO_S_ISREG(ent.d_stat.st_mode) ||
                   !has_mp3_ext(ent.d_name)) {
            continue;
        }

        r = snprintf(path + parent_len, sizeof(path) - parent_len,
                     "/%s", ent.d_name);
        if (r < 0 || r >= (int)(sizeof(path) - parent_len)) {
            path[parent_len] = 0;
            mlog("local: path too long, skip %s/%s", path, ent.d_name);
            continue;
        }
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            SceUID fd = sceIoDopen(path);
            if (fd >= 0) {
                ++depth;
                frames[depth].fd = fd;
                frames[depth].path_len = parent_len + (unsigned int)r;
                continue;
            }
        } else {
            /* FAT paths are case insensitive; keep the first full path. */
            for (i = 0; i < g_loc_n; ++i) {
                if (!strcasecmp(g_loc[i].path, path)) break;
            }
            if (i == g_loc_n) {
                memcpy(g_loc[g_loc_n].path, path, parent_len + (unsigned int)r + 1);
                snprintf(g_loc[g_loc_n].name, sizeof(g_loc[g_loc_n].name), "%s", ent.d_name);
                g_loc[g_loc_n].size = (int)ent.d_stat.st_size;
                ++g_loc_n;
            }
        }
        path[parent_len] = 0;
    }
    /* Also close every parent when the table filled inside a deep subtree. */
    while (depth >= 0) sceIoDclose(frames[depth--].fd);
}

int local_scan(void)
{
    if (g_loc_scanned) return g_loc_n;
    g_loc_scanned = 1;
    g_loc_n = 0;

    scan_dir("ms0:/MUSIC");          /* MELODY subtree first, visited once */
    scan_dir("ms0:/PSP/MUSIC");

    mlog("local music: %d files", g_loc_n);
    return g_loc_n;
}

void local_rescan(void)
{
    g_loc_scanned = 0;
    local_scan();
}

int local_count(void) { return g_loc_n; }

const char *local_path_of(int i)
{
    if (i < 0 || i >= g_loc_n) return NULL;
    return g_loc[i].path;
}

const char *local_name(int i)
{
    if (i < 0 || i >= g_loc_n) return NULL;
    return g_loc[i].name;
}

int local_size(int i)
{
    if (i < 0 || i >= g_loc_n) return 0;
    return g_loc[i].size;
}

/* ------------------------------------------------------------------ */
/* ID3v2.3                                                             */
/* ------------------------------------------------------------------ */

static int utf8_to_utf16le(const char *s, unsigned char *out, int max)
{
    int n = 0;

    while (*s) {
        unsigned int cp;
        unsigned char c = (unsigned char)*s;
        int len;

        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { s++; continue; }

        for (int i = 1; i < len; ++i) {
            if ((s[i] & 0xC0) != 0x80) { len = i; break; }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        s += len;

        if (cp >= 0x10000) {
            unsigned int v = cp - 0x10000;
            unsigned short hi = (unsigned short)(0xD800 + (v >> 10));
            unsigned short lo = (unsigned short)(0xDC00 + (v & 0x3FF));

            if (n + 4 > max) break;
            out[n++] = (unsigned char)(hi & 0xFF);
            out[n++] = (unsigned char)(hi >> 8);
            out[n++] = (unsigned char)(lo & 0xFF);
            out[n++] = (unsigned char)(lo >> 8);
        } else {
            if (n + 2 > max) break;
            out[n++] = (unsigned char)(cp & 0xFF);
            out[n++] = (unsigned char)(cp >> 8);
        }
    }
    return n;
}

/* 写一个文本帧；返回写入字节数 */
static int write_text_frame(FILE *fp, const char *id, const char *utf8)
{
    unsigned char text[512];
    unsigned char hdr[10];
    int n;
    int total;

    if (!utf8 || !utf8[0]) return 0;

    text[0] = 0x01;                      /* UTF-16 with BOM */
    text[1] = 0xFF;
    text[2] = 0xFE;
    n = utf8_to_utf16le(utf8, text + 3, (int)sizeof(text) - 3);
    if (n <= 0) return 0;

    total = n + 3;
    memcpy(hdr, id, 4);
    hdr[4] = (unsigned char)((total >> 24) & 0xFF);
    hdr[5] = (unsigned char)((total >> 16) & 0xFF);
    hdr[6] = (unsigned char)((total >> 8) & 0xFF);
    hdr[7] = (unsigned char)(total & 0xFF);
    hdr[8] = 0;
    hdr[9] = 0;

    fwrite(hdr, 1, 10, fp);
    fwrite(text, 1, (size_t)total, fp);
    return 10 + total;
}

/* 把封面塞进 ID3 的 APIC 帧（PSP 的 XMB 靠它显示封面） */
static int write_apic(FILE *fp, long long id)
{
    static unsigned char pic[48 * 1024];
    int n = cache_art_read(id, pic, sizeof(pic));
    const char *mime;
    int mlen, total;
    unsigned char hdr[10];

    if (n <= 0) return 0;
    /* 修复：读满 48KB 说明封面被截断了（cache_art_read 不告诉你文件还有多大），
     * 宁可这一帧不写，也不要把半张 JPEG 塞进 ID3 让 XMB 显示花屏。 */
    if (n >= (int)sizeof(pic)) {
        mlog("export: embedded cover >= %d bytes, skip APIC", (int)sizeof(pic));
        return 0;
    }

    mime = (pic[0] == 0x89 && pic[1] == 'P') ? "image/png" : "image/jpeg";
    mlen = (int)strlen(mime) + 1;

    /* enc(1) + mime + 0 + type(1) + desc 的空终止(1) + 数据 */
    total = 1 + mlen + 1 + 1 + n;

    memcpy(hdr, "APIC", 4);
    hdr[4] = (unsigned char)((total >> 24) & 0xFF);
    hdr[5] = (unsigned char)((total >> 16) & 0xFF);
    hdr[6] = (unsigned char)((total >> 8) & 0xFF);
    hdr[7] = (unsigned char)(total & 0xFF);
    hdr[8] = 0;
    hdr[9] = 0;

    fwrite(hdr, 1, 10, fp);
    fputc(0x00, fp);                     /* 编码：ISO-8859-1 */
    fwrite(mime, 1, (size_t)mlen, fp);   /* MIME + NUL */
    fputc(0x03, fp);                     /* 图片类型：封面正面 */
    fputc(0x00, fp);                     /* 描述：空 */
    fwrite(pic, 1, (size_t)n, fp);
    return 10 + total;
}

static int write_id3(FILE *fp, const Song *s)
{
    int size_pos = 0;
    int tag_start;
    int body;
    unsigned char head[10];
    unsigned char syncsafe[4];

    /* 标签总长要先知道，所以分两步：先占位写头，最后回填 */
    memcpy(head, "ID3", 3);
    head[3] = 3;      /* v2.3 */
    head[4] = 0;
    head[5] = 0;
    head[6] = head[7] = head[8] = head[9] = 0;
    if (fwrite(head, 1, 10, fp) != 10) return -1;

    tag_start = 10;
    body = write_text_frame(fp, "TIT2", s->name);
    if (body < 0) return -1;
    body += write_text_frame(fp, "TPE1", s->artist);
    body += write_text_frame(fp, "TALB", s->album);
    body += write_apic(fp, s->id);

    syncsafe[0] = (unsigned char)((body >> 21) & 0x7F);
    syncsafe[1] = (unsigned char)((body >> 14) & 0x7F);
    syncsafe[2] = (unsigned char)((body >> 7) & 0x7F);
    syncsafe[3] = (unsigned char)(body & 0x7F);

    if (fseek(fp, size_pos + 6, SEEK_SET) != 0) return -1;
    if (fwrite(syncsafe, 1, 4, fp) != 4) return -1;
    if (fseek(fp, tag_start + body, SEEK_SET) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 导出                                                                */
/* ------------------------------------------------------------------ */

static int sanitize(const char *in, char *out, int max)
{
    int n = 0;

    for (const char *p = in; *p && n < max - 1; ++p) {
        char c = *p;

        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') c = '_';
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

int local_export(const Song *s)
{
    char src[256];
    char tmp[256];
    static char buf[LOCAL_COPY_BUF];
    FILE *in = NULL;
    FILE *out = NULL;
    long long done = 0;
    long long total = 0;
    int ok = -1;
    int plen;
    int created = 0;             /* 本次是否真的创建了输出文件 */

    /* 修复：所有失败路径统一走 done，把 g_exp_active 清掉并写上 g_exp_ok，
     * 否则导出页会永远停在「正在导出」，之后的导出也全被挡住。 */
    if (!s) {
        g_exp_active = 0;
        g_exp_ok = -1;
        return -1;
    }
    g_exp_pct = 0;
    g_exp_ok = -1;
    g_exp_active = 1;
    g_exp_dst[0] = 0;

    plen = snprintf(src, sizeof(src), "./cache/%lld.mp3", s->id);
    if (plen < 0 || plen >= (int)sizeof(src)) {
        mlog("export: src path too long");
        ok = -3;
        goto done;
    }

    sceIoMkdir("ms0:/MUSIC", 0777);
    sceIoMkdir("ms0:/MUSIC/MELODY", 0777);

    /* 文件名用 ASCII，中文放 ID3 里（PSP 的 XMB 读 ID3） */
    plen = snprintf(g_exp_dst, sizeof(g_exp_dst), "ms0:/MUSIC/MELODY/MELODY-%lld.mp3", s->id);
    if (plen < 0 || plen >= (int)sizeof(g_exp_dst)) {
        mlog("export: dst path too long");
        ok = -3;
        goto done;
    }
    plen = snprintf(tmp, sizeof(tmp), "%s", g_exp_dst);
    if (plen < 0 || plen >= (int)sizeof(tmp)) {
        mlog("export: tmp path too long");
        ok = -3;
        goto done;
    }

    in = fopen(src, "rb");
    if (!in) {
        mlog("export: no cache file %s", src);
        ok = -1;
        goto done;
    }

    /* 先看空间：要放得下整首 + 1MB 余量 */
    {
        SceIoStat st;
        int free_mb = sysi_ms0_free_mb();

        if (sceIoGetstat(src, &st) < 0) {
            mlog("export: stat failed %s", src);
            ok = -1;
            goto done;
        }
        total = (long long)st.st_size;
        if (free_mb >= 0 && (long long)free_mb * 1024 * 1024 < total + 1024 * 1024) {
            mlog("export: not enough space (%dMB free, need %lldMB)", free_mb, total / 1024 / 1024 + 1);
            ok = -2;
            goto done;
        }
    }

    out = fopen(tmp, "wb");

    /* 这个文件可能正被播放器开着（真机 FAT 会拒绝），等它松手 */
    for (int i = 0; i < 6 && !out; ++i) {
        sceKernelDelayThread(150 * 1000);
        out = fopen(tmp, "wb");
    }

    if (!out) {
        mlog("export: cannot write %s", tmp);
        ok = -3;
        goto done;
    }
    created = 1;

    if (write_id3(out, s) < 0) {
        ok = -1;
        goto done;
    }

    /* 源文件自带的 ID3v2 标签跳过：否则导出文件里会有两个 ID3 头 */
    {
        unsigned char hdr[10];

        if (fread(hdr, 1, 10, in) == 10 && !memcmp(hdr, "ID3", 3)) {
            int sz = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                     ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);

            sz += 10;
            if (hdr[5] & 0x10) sz += 10;       /* footer */
            if (sz > 0 && sz < 1024 * 1024) {
                fseek(in, sz, SEEK_SET);
                mlog("export: skip source id3 %d", sz);
            } else {
                fseek(in, 0, SEEK_SET);
            }
        } else {
            fseek(in, 0, SEEK_SET);
        }
    }

    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), in);

        if (n == 0) break;
        if (fwrite(buf, 1, n, out) != n) {
            ok = -1;
            goto done;
        }
        done += (long long)n;
        if (total > 0) g_exp_pct = (int)(done * 100 / total);
    }

    ok = 0;
    g_exp_pct = 100;

done:
    if (out) fclose(out);
    if (in) fclose(in);

    /* 修复：早退时 g_exp_dst 可能是上一次导出成功的目标文件，
     * 只有本次确实创建过输出文件才允许删残文件。 */
    if (created && ok != 0) sceIoRemove(g_exp_dst);

    g_exp_active = 0;
    g_exp_ok = ok;
    mlog("export %lld -> %s : %s (%lldKB)", s->id, g_exp_dst[0] ? g_exp_dst : "-",
         ok == 0 ? "ok" : "fail", done / 1024);
    return ok;
}

int local_export_exists(long long id)
{
    char path[256];
    SceIoStat st;
    int n;

    n = snprintf(path, sizeof(path), "ms0:/MUSIC/MELODY/MELODY-%lld.mp3", id);
    if (n < 0 || n >= (int)sizeof(path)) return 0;
    return sceIoGetstat(path, &st) >= 0 && st.st_size > 64 * 1024;
}

int local_export_pct(void) { return g_exp_pct; }
int local_export_active(void) { return g_exp_active; }
int local_export_result(void) { return g_exp_ok; }
const char *local_export_path(void) { return g_exp_dst; }
void local_export_reset(void) { g_exp_ok = -1; g_exp_pct = 0; }
