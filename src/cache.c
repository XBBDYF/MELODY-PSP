#include "cache.h"

#include "log.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INDEX_PATH APP_DIR "cache/index.txt"

#define CID_MAX    24

typedef struct { long long id; int offset; int bytes; } CacheSlot;
#define SEEK_PATH APP_DIR "cache/index.seek"
#define LOOKUP_COUNT 32
typedef struct { long long id; int ordinal; int valid; } CacheLookup;
static CacheLookup g_lookup[LOOKUP_COUNT];
static unsigned int g_lookup_next;
#define ENTRY_RING 8
static CacheEntry g_entries[ENTRY_RING];
static unsigned int g_entry_next;
static int g_n;
static int g_gen;
static long long g_total;
static int g_lock = -1;

/* dl 线程会往里加歌，UI 在读 —— 用二值信号量护住 */
static void lock(void) { if (g_lock >= 0) sceKernelWaitSema(g_lock, 1, NULL); }
static void unlock(void) { if (g_lock >= 0) sceKernelSignalSema(g_lock, 1); }

/* ------------------------------------------------------------------ */
/* 路径                                                                */
/* ------------------------------------------------------------------ */

/* 路径统一由调用方给缓冲：以前三个函数共用一个 static 缓冲，UI/dl/api
 * 三个线程同时调用时会把路径改串（读/删到别的歌的文件）。 */
#define PATH_RING 8
static char g_path[PATH_RING][128];
static int g_path_i;

void cache_path(char *out, int n, int kind, long long id)
{
    static const char *ext[4] = { "mp3", "ok", "jpg", "lrc" };

    snprintf(out, (size_t)n, APP_DIR "cache/%lld.%s", id, ext[kind & 3]);
}

/* 兼容旧接口（ring buffer）：只在拿不到局部缓冲的地方用，且要立刻用掉 */
static const char *legacy_path(int kind, long long id)
{
    int i = g_path_i++ % PATH_RING;

    cache_path(g_path[i], (int)sizeof(g_path[i]), kind, id);
    return g_path[i];
}

const char *cache_file(long long id) { return legacy_path(0, id); }
const char *cache_ok_file(long long id) { return legacy_path(1, id); }
const char *cache_art_file(long long id) { return legacy_path(2, id); }

int cache_file_bytes(long long id)
{
    SceIoStat st;
    char p[128];

    cache_path(p, sizeof(p), 0, id);
    if (sceIoGetstat(p, &st) < 0) return -1;
    return (int)st.st_size;
}

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static void clean_field(char *s)
{
    for (; *s; ++s) {
        if (*s == '|' || *s == '\n' || *s == '\r' || *s == '\t') *s = ' ';
    }
}

/* 修复：读索引时把字段尾部的 \r\n 真正去掉。以前 art 字段带着换行，
 * 重启后拿它去请求封面就永远失败（store.c 的 clean_field 是换成空格，
 * 换成空格 URL 结尾同样被破坏，所以这里必须截断）。 */
static void trim_field(char *s)
{
    int n = (int)strlen(s);

    while (n > 0) {
        char c = s[n - 1];

        if (c != '\n' && c != '\r' && c != ' ' && c != '\t') break;
        s[--n] = 0;
    }
    /* 字段中间的换行（理论上不该有）也清掉 */
    for (char *p = s; *p; ++p)
        if (*p == '\n' || *p == '\r') *p = ' ';
}

static void write_ok(long long id)
{
    FILE *fp;
    char p[128];

    cache_path(p, sizeof(p), 1, id);
    io_lock();
    fp = fopen(p, "w");
    if (fp) { fputc('1', fp); fclose(fp); }
    io_unlock();
}

static int has_ok(long long id)
{
    FILE *fp;
    int ok = 0;
    char p[128];

    cache_path(p, sizeof(p), 1, id);
    io_lock();
    fp = fopen(p, "r");
    if (fp) { fclose(fp); ok = 1; }
    io_unlock();
    return ok;
}

static void drop_file(long long id)
{
    char p[128];

    cache_path(p, sizeof(p), 0, id); sceIoRemove(p);
    cache_path(p, sizeof(p), 1, id); sceIoRemove(p);
    cache_path(p, sizeof(p), 2, id); sceIoRemove(p);
    cache_path(p, sizeof(p), 3, id); remove(p);
}

/* ------------------------------------------------------------------ */
/* 索引                                                                */
/* ------------------------------------------------------------------ */

static int split_fields(char *line, char *f[], int max)
{
    int n = 0;
    char *p = line;

    f[n++] = p;
    while (*p && n < max) {
        if (*p == '|') {
            *p = 0;
            f[n++] = p + 1;
        }
        p++;
    }
    return n;
}


/* Metadata stays on disk; only IDs, byte counts and offsets are resident. */
static int parse_entry(char *line, CacheEntry *e)
{
    char *f[8];
    if (line[0] == '#' || split_fields(line, f, 8) < 7) return 0;
    memset(e, 0, sizeof(*e));
    e->id = atoll(f[0]); e->dur_ms = atoi(f[1]); e->bytes = atoi(f[2]);
    snprintf(e->name, sizeof(e->name), "%s", f[3]);
    snprintf(e->artist, sizeof(e->artist), "%s", f[4]);
    snprintf(e->album, sizeof(e->album), "%s", f[5]);
    snprintf(e->art, sizeof(e->art), "%s", f[6]);
    trim_field(e->name); trim_field(e->artist); trim_field(e->album); trim_field(e->art);
    return e->id > 0 && e->name[0];
}

/* This disposable disk table has one record per completed song, with no
 * capacity constant. The authoritative UTF-8 text index remains untouched. */
static void index_load(void)
{
    FILE *fp, *seek;
    char line[560];
    int ok = 1;
    g_n = 0; g_total = 0;
    memset(g_lookup, 0, sizeof(g_lookup)); g_lookup_next = 0;
    io_lock(); fp = fopen(INDEX_PATH, "r"); seek = fopen(SEEK_PATH, "wb"); io_unlock();
    if (!seek) { if (fp) fclose(fp); mlog("cache seek table open failed"); return; }
    if (fp) {
        for (;;) {
            long offset = ftell(fp);
            CacheEntry e;
            CacheSlot slot;
            int bytes;
            if (!fgets(line, sizeof(line), fp)) break;
            if (!parse_entry(line, &e)) continue;
            bytes = cache_file_bytes(e.id);
            if (bytes < 0 || !has_ok(e.id)) continue;
            slot.id = e.id; slot.offset = (int)offset; slot.bytes = bytes;
            if (fwrite(&slot, sizeof(slot), 1, seek) != 1) { ok = 0; break; }
            g_n++; if (bytes > 0) g_total += bytes;
        }
        if (ferror(fp)) ok = 0;
        fclose(fp);
    }
    if (fclose(seek)) ok = 0;
    if (!ok) { g_n = 0; g_total = 0; mlog("cache seek table write failed; media retained"); }
}

static int slot_find(long long id)
{
    CacheSlot slots[64];
    FILE *fp;
    int found = -1, ordinal = 0;
    size_t n;
    CacheLookup *item;
    for (int i = 0; i < LOOKUP_COUNT; i++)
        if (g_lookup[i].valid && g_lookup[i].id == id) return g_lookup[i].ordinal;
    io_lock(); fp = fopen(SEEK_PATH, "rb");
    if (fp) {
        while ((n = fread(slots, sizeof(slots[0]), 64, fp)) > 0) {
            for (size_t i = 0; i < n; i++) if (slots[i].id == id) { found = ordinal + (int)i; break; }
            if (found >= 0) break;
            ordinal += (int)n;
        }
        fclose(fp);
    }
    io_unlock();
    item = &g_lookup[g_lookup_next++ % LOOKUP_COUNT];
    item->id = id; item->ordinal = found; item->valid = 1;
    return found;
}

void cache_init(void)
{
    if (g_lock < 0) g_lock = sceKernelCreateSema("melody_cache", 0, 1, 1, NULL);
    sceIoMkdir(APP_DIR "cache", 0777);
    lock();
    /* Recover an interrupted index replacement; never touch media files. */
    { FILE *fp = fopen(INDEX_PATH, "r");
      if (fp) fclose(fp); else rename(INDEX_PATH ".bak", INDEX_PATH); }
    index_load(); g_gen++;
    unlock();
    mlog("cache: %d songs, %lldKB", g_n, g_total / 1024);
}

/* Every mutation commits its metadata before publishing offsets. */
void cache_save(void) { }

static int entry_read(int i, CacheEntry *out)
{
    FILE *fp, *seek;
    CacheSlot slot;
    char line[560];
    int ok = 0;
    if (i < 0 || i >= g_n) return 0;
    io_lock(); seek = fopen(SEEK_PATH, "rb");
    if (seek) {
        ok = !fseek(seek, (long)i * (long)sizeof(slot), SEEK_SET) && fread(&slot, sizeof(slot), 1, seek) == 1;
        fclose(seek);
    }
    if (ok) {
        ok = 0; fp = fopen(INDEX_PATH, "r");
        if (fp) {
            if (!fseek(fp, slot.offset, SEEK_SET) && fgets(line, sizeof(line), fp))
                ok = parse_entry(line, out) && out->id == slot.id;
            fclose(fp);
        }
    }
    io_unlock(); return ok;
}

int cache_count(void) { return g_n; }

const CacheEntry *cache_get(int i)
{
    CacheEntry *e;
    int ok;
    lock(); e = &g_entries[g_entry_next++ % ENTRY_RING]; ok = entry_read(i, e); unlock();
    return ok ? e : NULL;
}

const CacheEntry *cache_find(long long id)
{
    CacheEntry *e;
    int ok;
    lock(); e = &g_entries[g_entry_next++ % ENTRY_RING]; ok = entry_read(slot_find(id), e); unlock();
    return ok ? e : NULL;
}

int cache_has(long long id)
{
    int found; lock(); found = slot_find(id) >= 0; unlock(); return found;
}
int cache_complete(long long id) { return cache_has(id); }

int cache_snapshot_page(CacheEntry *out,int max,int first)
{
 int n=0;FILE*seek=NULL,*text=NULL;CacheSlot slot;char line[560];
 if(!out||max<=0||first<0)return 0;
 lock();io_lock();
 if(first<g_n){seek=fopen(SEEK_PATH,"rb");text=fopen(INDEX_PATH,"r");}
 if(seek&&text&&!fseek(seek,(long)first*(long)sizeof(slot),SEEK_SET)){
  while(n<max&&first+n<g_n&&fread(&slot,sizeof(slot),1,seek)==1){
   if(fseek(text,slot.offset,SEEK_SET)||!fgets(line,sizeof(line),text)||!parse_entry(line,&out[n])||out[n].id!=slot.id)break;
   n++;
  }
 }
 if(seek)fclose(seek);
 if(text)fclose(text);
 io_unlock();unlock();return n;
}
int cache_snapshot(CacheEntry *out, int max) { return cache_snapshot_page(out, max, 0); }

/* One generation-consistent playback queue, independent of future mutations.
 * Caller owns the destination; only publish it after the complete write closes. */
int cache_snapshot_ids(const char *path)
{
    FILE *src, *dst;
    CacheSlot slots[64];
    char temp[256], backup[256];
    int count = 0, ok = 1, had_destination = 0;
    size_t n;
    if (!path || snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp) ||
        snprintf(backup, sizeof(backup), "%s.bak", path) >= (int)sizeof(backup)) return -1;
    lock(); io_lock();
    src = fopen(SEEK_PATH, "rb"); dst = fopen(temp, "wb");
    if (!src || !dst) ok = 0;
    if (ok) {
        while ((n = fread(slots, sizeof(slots[0]), 64, src)) > 0)
            for (size_t i = 0; i < n; i++) {
                if (fwrite(&slots[i].id, sizeof(slots[i].id), 1, dst) != 1) ok = 0;
                count++;
            }
        if (ferror(src) || count != g_n) ok = 0;
    }
    if (src) fclose(src);
    if (dst && fclose(dst)) ok = 0;
    if (ok) {
        FILE *old = fopen(path, "rb");
        if (old) { had_destination = 1; fclose(old); }
        remove(backup);
        if (had_destination && rename(path, backup)) ok = 0;
        if (ok && rename(temp, path)) { if (had_destination) rename(backup, path); ok = 0; }
        if (ok) remove(backup);
    }
    if (!ok) remove(temp);
    io_unlock(); unlock(); return ok ? count : -1;
}

static int entry_write(FILE *fp, const CacheEntry *source)
{
    CacheEntry e = *source;
    clean_field(e.name); clean_field(e.artist); clean_field(e.album); clean_field(e.art);
    return fprintf(fp, "%lld|%d|%d|%s|%s|%s|%s\n", e.id, e.dur_ms, e.bytes,
                   e.name, e.artist, e.album, e.art) >= 0;
}

/* Stream the original file, preserving records beyond the resident capacity too.
 * The old file remains readable until a successful close and rename. */
static int index_commit(long long id, const CacheEntry *replacement, int clear)
{
    FILE *src, *dst;
    char line[560];
    int ok = 1, replaced = 0, had_source;
    io_lock();
    src = fopen(INDEX_PATH, "r"); had_source = src != NULL;
    dst = fopen(INDEX_PATH ".tmp", "w");
    if (!dst) { if (src) fclose(src); io_unlock(); return 0; }
    if (src) {
        while (fgets(line, sizeof(line), src)) {
            if (clear) continue;
            if (line[0] != '#' && atoll(line) == id) {
                if (replacement && !replaced) ok &= entry_write(dst, replacement);
                replaced = 1;
            } else if (fputs(line, dst) == EOF) ok = 0;
        }
        if (ferror(src)) ok = 0;
        fclose(src);
    }
    if (replacement && !replaced) ok &= entry_write(dst, replacement);
    if (fclose(dst)) ok = 0;
    if (ok) {
        /* Keep a recovery copy across the two FAT renames. */
        remove(INDEX_PATH ".bak");
        if (rename(INDEX_PATH, INDEX_PATH ".bak") && had_source) ok = 0;
        if (ok && rename(INDEX_PATH ".tmp", INDEX_PATH)) {
            rename(INDEX_PATH ".bak", INDEX_PATH); ok = 0;
        }
        if (ok) remove(INDEX_PATH ".bak");
    }
    if (!ok) remove(INDEX_PATH ".tmp");
    io_unlock();
    if (ok) { index_load(); g_gen++; }
    return ok;
}

void cache_add(const Song *s, int bytes)
{
    CacheEntry e;
    if (!s || s->id <= 0) return;
    lock();
    memset(&e, 0, sizeof(e)); e.id = s->id; e.dur_ms = s->dur_ms;
    e.bytes = bytes > 0 ? bytes : cache_file_bytes(s->id);
    snprintf(e.name, sizeof(e.name), "%s", s->name);
    snprintf(e.artist, sizeof(e.artist), "%s", s->artist);
    snprintf(e.album, sizeof(e.album), "%s", s->album);
    snprintf(e.art, sizeof(e.art), "%s", s->art);
    write_ok(s->id);
    if (!index_commit(s->id, &e, 0)) mlog("cache index commit failed; media retained");
    unlock();
}

void cache_adopt(long long id, const char *name, const char *artist, int dur_ms, int bytes)
{
    Song s;
    if (id <= 0 || cache_has(id)) return;
    memset(&s, 0, sizeof(s)); s.id = id; s.dur_ms = dur_ms;
    snprintf(s.name, sizeof(s.name), "%s", name ? name : "");
    snprintf(s.artist, sizeof(s.artist), "%s", artist ? artist : "");
    cache_add(&s, bytes);
}

void cache_remove(long long id)
{
    lock();
    if (slot_find(id) >= 0 && index_commit(id, NULL, 0)) drop_file(id);
    unlock();
}

void cache_clear(void)
{
    lock();
    /* Explicit user clear only. Do not prune partial downloads. */
    while (g_n > 0) {
        CacheEntry entry;
        long long id;
        if (!entry_read(0, &entry)) break;
        id = entry.id;
        if (!index_commit(id, NULL, 0)) break;
        drop_file(id);
    }
    unlock();
}

long long cache_total_bytes(void) { return g_total; }
int cache_gen(void) { return g_gen; }

/* ------------------------------------------------------------------ */
/* 封面缓存                                                            */
/* ------------------------------------------------------------------ */

int cache_art_read(long long id, unsigned char *buf, int max)
{
    FILE *fp;
    int n;
    char p[128];

    cache_path(p, sizeof(p), 2, id);
    io_lock();
    fp = fopen(p, "rb");
    if (!fp) {
        io_unlock();
        return -1;
    }
    n = (int)fread(buf, 1, (size_t)max, fp);
    fclose(fp);
    io_unlock();
    return n > 0 ? n : -1;
}

void cache_art_drop(long long id)
{
    char p[128];

    cache_path(p, sizeof(p), 2, id);
    io_lock();
    remove(p);
    io_unlock();
}

int cache_lyric_read(long long id, char *buf, int max)
{
    FILE *fp;
    int n;
    char p[128];

    cache_path(p, sizeof(p), 3, id);
    io_lock();
    fp = fopen(p, "rb");
    if (!fp) {
        io_unlock();
        return -1;
    }
    n = (int)fread(buf, 1, (size_t)(max - 1), fp);
    fclose(fp);
    io_unlock();

    if (n <= 0) return -1;
    buf[n] = 0;
    return n;
}

int cache_lyric_write(long long id, const char *buf, int len)
{
    FILE *fp;
    char p[128];

    cache_path(p, sizeof(p), 3, id);
    io_lock();
    fp = fopen(p, "wb");
    if (!fp) {
        io_unlock();
        return -1;
    }
    fwrite(buf, 1, (size_t)len, fp);
    fclose(fp);
    io_unlock();
    return 0;
}

void cache_lyric_remove(long long id)
{
    char p[128];

    cache_path(p, sizeof(p), 3, id);
    io_lock();
    remove(p);
    io_unlock();
}

int cache_art_write(long long id, const unsigned char *buf, int len)
{
    FILE *fp;
    char p[128];

    if (!buf || len <= 0) return -1;
    cache_path(p, sizeof(p), 2, id);
    io_lock();
    fp = fopen(p, "wb");
    if (!fp) {
        io_unlock();
        return -1;
    }
    {
        size_t wrote = fwrite(buf, 1, (size_t)len, fp);
        int closed = fclose(fp);
        if (wrote != (size_t)len || closed != 0) {
            remove(p); /* Disposable cache: never publish a partial image. */
            io_unlock();
            return -1;
        }
    }
    io_unlock();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 清理残留                                                            */
/* ------------------------------------------------------------------ */

static int name_is_mp3(const char *name)
{
    size_t l = strlen(name);

    return l > 4 && !strcasecmp(name + l - 4, ".mp3");
}

void cache_prune(void)
{
    /* Incomplete media may be resumable even without a checkpoint. Capacity
     * maintenance must never destroy it; explicit remove/clear own deletion. */
    int dfd, retained = 0;
    SceIoDirent d;
    dfd = sceIoDopen(APP_DIR "cache");
    if (dfd < 0) return;
    memset(&d, 0, sizeof(d));
    while (sceIoDread(dfd, &d) > 0) {
        if (name_is_mp3(d.d_name)) retained++;
        memset(&d, 0, sizeof(d));
    }
    sceIoDclose(dfd);
    mlog("cache prune: retained %d media files", retained);
}
