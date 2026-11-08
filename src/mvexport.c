#include "thread_paths.h"
/*
 * MELODY PSP — MV（视频）导出实现
 * ------------------------------------------------------------------
 * 流程（全部在调用线程同步执行，约定是 API 线程）：
 *   1. 检查 cache/mv_<id>_240.mp4 存在且 > 0；
 *   2. 生成目标名 MELODY-<id>_<歌手> - <标题>.MP4（ASCII 清洗 + 截断）；
 *   3. 空间检查：不够但存在同名旧文件时，先删旧文件腾地方；
 *   4. 复制到固定临时文件 ms0:/VIDEO/MELODY-EXPORT.TMP（32KB 块、短写循环、
 *      每 256KB 刷新一次进度、响应取消）；
 *   5. 校验临时文件大小 == 源大小，删旧、rename 成最终名、sceIoSync 落盘；
 *   6. 失败/取消一律删临时文件；成功写 cache/mvexport_<id>.txt 指针。
 *
 * 为什么不直接改 cache 里的文件名：缓存要保留（离线播放 / MV 列表 / 删除语义），
 * 而 FAT 没有硬链接，所以只能复制；但临时文件和最终文件同目录，末端 rename 很便宜。
 */

#include "mvexport.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "log.h"
#include "sysinfo.h"

#define MVX_SRC_FMT   APP_DIR "cache/mv_%d_240.mp4"
#define MVX_PTR_FMT   APP_DIR "cache/mvexport_%d.txt"
/* 空间余量：簇对齐（32KB）+ 目录项，避免刚好差几 KB 写到一半失败 */
#define MVX_SLACK     (2 * 1024 * 1024)
/* 指针文件只存一行文件名；超过这个长度一定是坏文件 */
#define MVX_PTR_BYTES 128

/* ------------------------------------------------------------------ */
/* 状态（UI 只读，全部 volatile 的都在这里）                            */
/* ------------------------------------------------------------------ */

/* 注意：访问器叫 mvx_pct()，C 里函数和变量不能同名，所以计数器用 g_ 前缀
 * （和项目里 g_exp_pct 的风格一致）。 */
static volatile int g_mvx_pct;
static volatile int g_mvx_active;
static volatile int mvx_worker_live;
static volatile int g_mvx_cancel;

static int g_mvx_result = -1;
static int g_mvx_mvid;
static char g_mvx_path[MVX_PATH_MAX];

/* 32KB 拷贝缓冲放 BSS（不进 2MB 堆，MV/API 那两块大缓冲很紧） */
static unsigned char g_mvx_buf[MVX_BLOCK];

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static int mvx_append(char *buf, int cap, int pos, const char *s)
{
    if (!s) return pos;
    while (*s && pos < cap - 1) buf[pos++] = *s++;
    buf[pos] = 0;
    return pos;
}

/* 清洗一段名字：只留可打印 ASCII，非法字符 / 控制字符 / 非 ASCII 一律变 '_'，
 * 连续 '_' 合并，去掉首尾空格和结尾的点/下划线（Windows 不接受结尾的点）。
 * 返回写入长度；全被清掉时返回 0 且 out 为空串。 */
static int mvx_clean(const char *in, char *out, int max)
{
    int n = 0;
    int last_us = 1;        /* 开头不能是 '_' */

    if (max <= 0) return 0;
    out[0] = 0;
    if (!in) return 0;

    while (*in && n < max - 1) {
        unsigned char c = (unsigned char)*in++;
        char o;

        if (c < 0x20 || c >= 0x7F) o = '_';                    /* 控制字符 / 非 ASCII */
        else if (strchr("/\\:*?\"<>|", (int)c)) o = '_';       /* FAT / Windows 非法字符 */
        else o = (char)c;

        if (o == '_') {
            if (last_us) continue;                             /* 合并连续下划线 */
            last_us = 1;
        } else {
            if (o == ' ' && n == 0) continue;                  /* 去前导空格 */
            last_us = 0;
        }
        out[n++] = o;
    }

    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.' || out[n - 1] == '_')) --n;
    out[n] = 0;
    return n;
}

/* MELODY-<mvid>_<artist> - <name>.MP4；artist ≤ 24 字节、name ≤ 40 字节，
 * 整名 ≤ MVX_NAME_MAX（调用方给 cap = MVX_NAME_MAX + 1）。返回文件名长度。 */
static int mvx_build_name(char *out, int cap, int mvid, const char *name, const char *artist)
{
    char a[25];
    char nm[41];
    char id[16];
    int pos = 0;

    mvx_clean(artist, a, (int)sizeof(a));
    mvx_clean(name, nm, (int)sizeof(nm));
    snprintf(id, sizeof(id), "%d", mvid);

    out[0] = 0;
    pos = mvx_append(out, cap, pos, "MELODY-");
    pos = mvx_append(out, cap, pos, id);
    if (a[0]) {
        pos = mvx_append(out, cap, pos, "_");
        pos = mvx_append(out, cap, pos, a);
    }
    if (nm[0]) {
        pos = mvx_append(out, cap, pos, " - ");
        pos = mvx_append(out, cap, pos, nm);
    }
    pos = mvx_append(out, cap, pos, MVX_EXT);
    return pos;
}

/* MVX_DIR "/" base → out；超长返回 -1（正常不会，路径上限 128 是硬约束）。 */
static int mvx_make_path(char *out, int cap, const char *base)
{
    int dir_len = (int)strlen(MVX_DIR);
    int base_len = (int)strlen(base);

    if (dir_len + 1 + base_len + 1 > cap) return -1;
    if (dir_len + 1 + base_len >= MVX_PATH_MAX) return -1;

    memcpy(out, MVX_DIR, (size_t)dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, base, (size_t)base_len + 1);
    return 0;
}

static void mvx_ptr_path(char *out, int cap, int mvid)
{
    out[0] = 0;
    snprintf(out, (size_t)cap, MVX_PTR_FMT, mvid);
}

/* 记住最终文件名：mvx_exists() 靠它，不需要扫目录。写失败不影响导出结果。 */
static void mvx_ptr_write(const char *ptr, const char *base)
{
    int fd = sceIoOpen(ptr, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);

    if (fd < 0) return;
    sceIoWrite(fd, base, (SceSize)strlen(base));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}

/* 读回指针文件的第一行；没有/空文件返回 0。 */
static int mvx_ptr_read(const char *ptr, char *out, int cap)
{
    int fd;
    int n;
    char *e;

    if (cap < 2) return 0;
    out[0] = 0;

    fd = sceIoOpen(ptr, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    n = sceIoRead(fd, out, (SceSize)(cap - 1));
    sceIoClose(fd);
    if (n <= 0) {
        out[0] = 0;
        return 0;
    }
    out[n] = 0;
    e = strchr(out, '\n');
    if (e) *e = 0;
    e = strchr(out, '\r');
    if (e) *e = 0;
    return out[0] ? 1 : 0;
}

/* 复制循环：32KB 读、短写补写、每 256KB 刷进度、每块查取消。
 * 返回 0 成功 / -3 读写失败或长度不符 / -4 被取消。 */
static int mvx_copy(int in, int out, long long total)
{
    long long done = 0;
    long long next_pct = MVX_PCT_STEP;

    for (;;) {
        int n = sceIoRead(in, g_mvx_buf, (SceSize)sizeof(g_mvx_buf));
        int off = 0;

        if (n < 0) return -3;
        if (n == 0) break;

        /* sceIoWrite 在真机 FAT 上可能短写，必须补完这一块才算数 */
        while (off < n) {
            int w = sceIoWrite(out, g_mvx_buf + off, (SceSize)(n - off));

            if (w <= 0) return -3;
            off += w;
        }

        done += n;
        if (done >= next_pct) {
            int pct = total > 0 ? (int)((done * 100) / total) : 0;

            /* 复制阶段最多报 99%：100% 只留给 rename/sync 都成功之后 */
            if (pct > 99) pct = 99;
            g_mvx_pct = pct;
            next_pct = (done / MVX_PCT_STEP + 1) * MVX_PCT_STEP;
        }

        if (g_mvx_cancel) return -4;
    }

    if (done != total) return -3;      /* 源文件比 stat 时短：不报成功 */
    return 0;
}

/* ------------------------------------------------------------------ */
/* 导出                                                                */
/* ------------------------------------------------------------------ */

int mvx_export(int mvid, const char *name, const char *artist)
{
    char src[MVX_PATH_MAX];
    char base[MVX_NAME_MAX + 1];
    char dst[MVX_PATH_MAX];
    char ptr[MVX_PATH_MAX];
    char old_base[MVX_PTR_BYTES];
    char old_dst[MVX_PATH_MAX];
    SceIoStat st;
    long long total;
    int in = -1;
    int out = -1;
    int free_mb = -1;
    int removed_old = 0;
    int has_old = 0;
    int r = -3;
    int len;

    /* 同一时刻只允许一个导出（临时文件是固定名）。正常调用方只有 API 线程，
     * 这里只是防御。 */
    if (g_mvx_active) {
        mlog("mvexport %d: busy, ignored", mvid);
        return -3;
    }

    g_mvx_pct = 0;
    g_mvx_result = -1;
    g_mvx_active = 1;
    g_mvx_cancel = 0;
    g_mvx_mvid = mvid;
    g_mvx_path[0] = 0;

    if (mvid <= 0) {
        mlog("mvexport: bad mvid %d", mvid);
        r = -1;
        goto done;
    }

    /* ---- 源文件 ---- */
    {
        int n = snprintf(src, sizeof(src), MVX_SRC_FMT, mvid);

        if (n < 0 || n >= (int)sizeof(src)) {
            mlog("mvexport %d: src path too long", mvid);
            r = -1;
            goto done;
        }
    }
    if (sceIoGetstat(src, &st) < 0 || (long long)st.st_size <= 0) {
        /* 没下完（只有 .part）和完全没下过都走这里 */
        mlog("mvexport %d: no source %s", mvid, src);
        r = -1;
        goto done;
    }
    total = (long long)st.st_size;

    /* ---- 目标名 / 路径 / 指针文件路径 ---- */
    len = mvx_build_name(base, (int)sizeof(base), mvid, name, artist);
    if (len <= 0 || len > MVX_NAME_MAX) {
        mlog("mvexport %d: bad name len=%d", mvid, len);
        r = -3;
        goto done;
    }
    if (mvx_make_path(dst, (int)sizeof(dst), base) < 0) {
        mlog("mvexport %d: dst path too long", mvid);
        r = -3;
        goto done;
    }
    mvx_ptr_path(ptr, (int)sizeof(ptr), mvid);

    /* 上次导出过、且这次标题/歌手算出来的名字不一样时，完成后要把旧名字那份
     * 删掉：同一个 MV 只留一个导出文件（两个入口传的标题可能不同）。 */
    if (mvx_ptr_read(ptr, old_base, (int)sizeof(old_base)) &&
        strncmp(old_base, "MELODY-", 7) == 0 &&
        !strchr(old_base, '/') && !strchr(old_base, '\\') &&
        mvx_make_path(old_dst, (int)sizeof(old_dst), old_base) == 0) {
        has_old = 1;
    }

    /* ---- 空间检查（-1 = 模拟器/devctl 不支持：跳过） ---- */
    sysi_update();
    free_mb = sysi_ms0_free_mb();
    if (free_mb >= 0) {
        long long free_b = (long long)free_mb * 1024 * 1024;

        if(free_b<total+MVX_SLACK){r=-2;goto done;}
    }

    mlog("mvexport %d: %lldKB -> %s (free=%dMB)", mvid, total / 1024, dst, free_mb);

    /* ---- 复制到固定临时文件 ---- */
    sceIoMkdir(MVX_DIR, 0777);         /* 已存在会返回 <0，忽略 */
    sceIoRemove(MVX_TMP);              /* 清上次硬关机留下的残留文件 */

    in = sceIoOpen(src, PSP_O_RDONLY, 0);
    if (in < 0) {
        mlog("mvexport %d: open src failed", mvid);
        r = -1;
        goto done;
    }
    out = sceIoOpen(MVX_TMP, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (out < 0) {
        mlog("mvexport %d: open tmp failed %s", mvid, MVX_TMP);
        r = -3;
        goto done;
    }

    r = mvx_copy(in, out, total);
    sceIoClose(out);
    out = -1;
    sceIoClose(in);
    in = -1;

    if (r == -4) {
        mlog("mvexport %d: cancelled at %d%%", mvid, g_mvx_pct);
        goto done;
    }
    if (r != 0) {
        mlog("mvexport %d: copy failed rc=%d at %d%%", mvid, r, g_mvx_pct);
        goto done;
    }

    /* ---- 校验临时文件，覆盖旧文件，改名 ---- */
    {
        SceIoStat tmp;

        if (sceIoGetstat(MVX_TMP, &tmp) < 0 || (long long)tmp.st_size != total) {
            mlog("mvexport %d: tmp size mismatch", mvid);
            r = -3;
            goto done;
        }
    }

    {
        SceIoStat old;

        if (sceIoGetstat(dst, &old) >= 0) {
            if (sceIoRemove(dst) < 0) {
                mlog("mvexport %d: remove old failed %s", mvid, dst);
                r = -3;
                goto done;
            }
            removed_old = 1;
        }
    }

    if (sceIoRename(MVX_TMP, dst) < 0) {
        mlog("mvexport %d: rename failed %s", mvid, dst);
        r = -3;
        goto done;
    }

    /* 刚写完就报成功，PC/PSP 枚举时可能看不到（真机 FAT 的写回有延迟） */
    sceIoSync("ms0:", 0);
    g_mvx_pct = 100;
    memcpy(g_mvx_path, dst, strlen(dst) + 1);
    /* 旧名字那份（如果和这次不同）现在可以删了：新文件已经就位 */
    if (has_old && strcmp(old_dst, dst) != 0) {
        sceIoRemove(old_dst);
        mlog("mvexport %d: dropped stale %s", mvid, old_dst);
    }
    mvx_ptr_write(ptr, base);

done:
    if (out >= 0) sceIoClose(out);
    if (in >= 0) sceIoClose(in);

    if (r != 0) {
        g_mvx_path[0] = 0;
        sceIoRemove(MVX_TMP);
        /* 旧文件已经被我们删掉的话，指针也不能再留着 */
        if (removed_old) sceIoRemove(ptr);
    }

    g_mvx_active = 0;
    g_mvx_result = r;
    g_mvx_cancel = 0;
    mlog("mvexport %d: %s rc=%d pct=%d path=%s", mvid, r == 0 ? "ok" : "fail", r,
         g_mvx_pct, g_mvx_path[0] ? g_mvx_path : "-");
    return r;
}

/* ------------------------------------------------------------------ */
/* 查询 / 控制                                                         */
/* ------------------------------------------------------------------ */

int mvx_pct(void) { return g_mvx_pct; }
int mvx_active(void) { return g_mvx_active||mvx_worker_live; }
int mvx_result(void) { return g_mvx_result; }
int mvx_mvid(void) { return g_mvx_mvid; }

const char *mvx_path(void)
{
    return g_mvx_path;
}

int mvx_exists(int mvid)
{
    char ptr[MVX_PATH_MAX];
    char base[MVX_PTR_BYTES];
    char dst[MVX_PATH_MAX];
    SceIoStat st;

    if (mvid <= 0) return 0;

    mvx_ptr_path(ptr, (int)sizeof(ptr), mvid);
    if (!mvx_ptr_read(ptr, base, (int)sizeof(base))) return 0;
    /* 指针文件只该有我们写的 “MELODY-…MP4”，别的一律当损坏 */
    if (strncmp(base, "MELODY-", 7) != 0) return 0;
    if (strchr(base, '/') || strchr(base, '\\')) return 0;
    if (mvx_make_path(dst, (int)sizeof(dst), base) < 0) return 0;
    if (sceIoGetstat(dst, &st) < 0) return 0;

    return (long long)st.st_size > 0;
}

void mvx_cancel(void)
{
    g_mvx_cancel = 1;
}

void mvx_cleanup(void)
{
    /* 固定名字 ⇒ 不需要枚举目录；没有这个文件时 sceIoRemove 静默失败 */
    sceIoRemove(MVX_TMP);
}

static int mvx_start_id;static char mvx_start_name[72],mvx_start_artist[64];
static int mvx_worker(SceSize args,void*argp){
#if defined(__PSP__)
    melody_bind_thread_directory();
#endif
(void)args;(void)argp;mvx_export(mvx_start_id,mvx_start_name,mvx_start_artist);mvx_worker_live=0;sceKernelExitDeleteThread(0);return 0;}
int mvx_start(int id,const char*name,const char*artist){
    if(id<=0||mvx_worker_live||mvx_active())return -1;
    char source[160];SceIoStat st;snprintf(source,sizeof(source),MVX_SRC_FMT,id);if(sceIoGetstat(source,&st)<0||st.st_size<=0)return -1;
    mvx_start_id=id;snprintf(mvx_start_name,sizeof(mvx_start_name),"%s",name?name:"");snprintf(mvx_start_artist,sizeof(mvx_start_artist),"%s",artist?artist:"");
    mvx_worker_live=1;SceUID thread=sceKernelCreateThread("mv_export",mvx_worker,0x28,32768,PSP_THREAD_ATTR_USER,NULL);
    if(thread<0){mvx_worker_live=0;return -1;}
    int result=sceKernelStartThread(thread,0,NULL);if(result<0){sceKernelDeleteThread(thread);mvx_worker_live=0;return -1;}return 0;
}
