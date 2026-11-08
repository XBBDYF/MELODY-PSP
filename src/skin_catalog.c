/* COS skin catalog. All mutations run on the application's sole API worker. */
#include "skin_catalog.h"
#include "json.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SKIN_HOST_TEST
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
/* Host builds provide APP_DIR and a validator stub, without the PSP headers. */
extern int skin_runtime_validate(const char *root, char *err, int n);
#else
#include "app.h"
#include "skin_runtime.h"
#include "log.h"
#include "thread_paths.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#endif

/* Memory-stick file operations are not always atomic under load: a directory
 * that exists can transiently fail sceIoGetstat while the MV stream worker is
 * writing megabytes, and a freshly created directory can reject the first open.
 * The old code turned each of those into a permanent "Cannot ..." failure and
 * left .new/.old staging directories behind. Every mutation is retried a few
 * times and the failing operation/path is logged so the next log is conclusive. */
#ifndef SKIN_HOST_TEST
#define SC_LOG(...) mlog(__VA_ARGS__)
#else
#define SC_LOG(...) ((void)0)
#endif

static void sc_pause(int attempt)
{
#ifdef SKIN_HOST_TEST
    (void)attempt;
#else
    sceKernelDelayThread((attempt + 1) * 25 * 1000);
#endif
}

/* Skin I/O runs on the API worker; re-bind on entry because that worker is torn
 * down and rebuilt around MV playback. */
static void sc_bind(void)
{
#ifndef SKIN_HOST_TEST
    (void)melody_bind_thread_directory();
#endif
}

#define SC_PATH 512
static SkinCatalogEntry g_entries[SKIN_CATALOG_MAX];
static int g_count;
static volatile int g_state, g_progress;
static char g_error[192];
/* Last install/apply failure. Unlike g_error it is not cleared by the preview or
 * catalog requests that follow, so the skin page can keep explaining why the
 * theme the user picked is still not active. */
static char g_install_error[192];

static int fail(const char *message)
{
    if (message != g_error) snprintf(g_error, sizeof(g_error), "%s", message);
    if (!g_error[0]) snprintf(g_error, sizeof(g_error), "Skin operation failed");
    if (g_state == SKIN_CATALOG_INSTALLING)
        snprintf(g_install_error, sizeof(g_install_error), "%s", g_error);
    SC_LOG("skin: %s", g_error);
    g_state = SKIN_CATALOG_ERROR;
    return -1;
}

static void begin(int state)
{
    g_error[0] = 0;
    g_progress = 0;
    if (state == SKIN_CATALOG_INSTALLING) g_install_error[0] = 0;
    g_state = state;
}

static int done(void)
{
    g_progress = 100;
    if (g_state == SKIN_CATALOG_INSTALLING) g_install_error[0] = 0;
    g_state = SKIN_CATALOG_READY;
    return 0;
}

static int valid_id(const char *id)
{
    int n = 0;
    if (!id || !id[0]) return 0;
    for (; id[n]; n++) {
        unsigned char c = (unsigned char)id[n];
        if (n >= 32 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* PSP memory sticks use case-insensitive names; aliases cannot coexist. */
static int same_id(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

static int path_join(char *out, int n, const char *root, const char *leaf)
{
    int len = snprintf(out, (size_t)n, "%s/%s", root, leaf);
    return len >= 0 && len < n ? 0 : -1;
}

static int fs_kind(const char *path)
{
#ifdef SKIN_HOST_TEST
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
#else
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0) return 0;
    return FIO_S_ISDIR(st.st_mode) ? 2 : 1;
#endif
}

static int fs_mkdir(const char *path)
{
    int attempt, rc = 0;
    if (fs_kind(path) == 2) return 0;
    for (attempt = 0; attempt < 3; ++attempt) {
#ifdef SKIN_HOST_TEST
#ifdef _WIN32
        rc = _mkdir(path);
#else
        rc = mkdir(path, 0777);
#endif
#else
        rc = sceIoMkdir(path, 0777);
#endif
        /* The directory can appear while we are retrying (another attempt won). */
        if (rc >= 0 || fs_kind(path) == 2) return 0;
        if (attempt < 2) sc_pause(attempt);
    }
    /* The raw SDK code matters: 0x80020320 is MFILE (descriptor exhaustion),
     * a path/cwd problem looks different. Keep it in the log verbatim. */
    SC_LOG("skin io: mkdir failed path=%s rc=0x%08X", path, (unsigned)rc);
    return -1;
}

static int fs_remove(const char *path)
{
    int attempt, rc = 0;
    for (attempt = 0; attempt < 3; ++attempt) {
#ifdef SKIN_HOST_TEST
        rc = remove(path);
#else
        rc = sceIoRemove(path);
#endif
        if (rc >= 0 || !fs_kind(path)) return 0;
        if (attempt < 2) sc_pause(attempt);
    }
    SC_LOG("skin io: remove failed path=%s rc=0x%08X", path, (unsigned)rc);
    return -1;
}

static int fs_rmdir(const char *path)
{
    int attempt, rc = 0;
    for (attempt = 0; attempt < 3; ++attempt) {
#ifdef SKIN_HOST_TEST
#ifdef _WIN32
        rc = _rmdir(path);
#else
        rc = rmdir(path);
#endif
#else
        rc = sceIoRmdir(path);
#endif
        if (rc >= 0 || !fs_kind(path)) return 0;
        if (attempt < 2) sc_pause(attempt);
    }
    SC_LOG("skin io: rmdir failed path=%s rc=0x%08X", path, (unsigned)rc);
    return -1;
}

/* Rename is the publication step: a single injected failure must still be
 * reported (and rolled back), so this one is deliberately not retried. */
static int fs_rename(const char *from, const char *to)
{
#ifdef SKIN_HOST_TEST
    int rc = rename(from, to);
#else
    int rc = sceIoRename(from, to);
#endif
    if (rc >= 0) return 0;
    SC_LOG("skin io: rename failed from=%s to=%s rc=0x%08X", from, to, (unsigned)rc);
    return -1;
}

static int write_file_once(const char *path, const char *data, int n)
{
#ifdef SKIN_HOST_TEST
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return -1;
    ok = fwrite(data, 1, (size_t)n, f) == (size_t)n;
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
#else
    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    int pos = 0, ok = 1;
    if (fd < 0) return -1;
    while (pos < n) {
        int wrote = sceIoWrite(fd, data + pos, n - pos);
        if (wrote <= 0) { ok = 0; break; }
        pos += wrote;
    }
    if (sceIoClose(fd) < 0) ok = 0;
    return ok ? 0 : -1;
#endif
}

/* A partial file is discarded before each retry so a short write can never be
 * mistaken for the real manifest/asset. */
static int write_file(const char *path, const char *data, int n)
{
    int attempt;
    for (attempt = 0; attempt < 3; ++attempt) {
        if (write_file_once(path, data, n) == 0) return 0;
        fs_remove(path);
        if (attempt < 2) sc_pause(attempt);
    }
    SC_LOG("skin io: write failed path=%s bytes=%d", path, n);
    return -1;
}
/* Resource names are flat ASCII IDs with an exact type extension. */
static int asset_name(const char *name)
{
    char id[33];
    size_t n = strlen(name);
    if (n < 5 || n > 36 || (strcmp(name + n - 4, ".png") && strcmp(name + n - 4, ".wav"))) return 0;
    memcpy(id, name, n - 4); id[n - 4] = 0;
    return valid_id(id);
}

static int clear_asset_child(const char *root, const char *name)
{
    char path[SC_PATH];
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    if (!asset_name(name) || path_join(path, sizeof(path), root, name) < 0 || fs_kind(path) != 1) return -1;
    return fs_remove(path);
}

/* Unknown filenames and subdirectories are retained and block removal. */
static int clear_assets(const char *skin)
{
    char root[SC_PATH];
    int bad = 0;
    if (path_join(root, sizeof(root), skin, "assets") < 0) return -1;
    if (!fs_kind(root)) return 0;
    if (fs_kind(root) != 2) return -1;
#ifdef SKIN_HOST_TEST
    {
        DIR *dir = opendir(root);
        struct dirent *de;
        if (!dir) return -1;
        while ((de = readdir(dir)) != NULL)
            if (clear_asset_child(root, de->d_name) < 0) bad = 1;
        closedir(dir);
    }
#else
    {
        int dir = sceIoDopen(root), rc;
        SceIoDirent de;
        if (dir < 0) return -1;
        for (;;) {
            memset(&de, 0, sizeof(de));
            rc = sceIoDread(dir, &de);
            if (rc <= 0) { if (rc < 0) bad = 1; break; }
            if (clear_asset_child(root, de.d_name) < 0) bad = 1;
        }
        sceIoDclose(dir);
    }
#endif
    return bad ? -1 : fs_rmdir(root);
}

/* Only remove files this module owns. Unknown content blocks replacement. */
static int clear_skin_dir(const char *root)
{
    static const char *files[] = { "skin.json", "skin.lua", "preview.png" };
    char path[SC_PATH];
    int i;
    if (!fs_kind(root)) return 0;
    if (fs_kind(root) != 2 || clear_assets(root) < 0) return -1;
    for (i = 0; i < 3; i++) {
        if (path_join(path, sizeof(path), root, files[i]) < 0) return -1;
        if (fs_kind(path) && fs_remove(path) < 0) return -1;
    }
    return fs_rmdir(root);
}

int skin_catalog_root(const char *id, char *out, int n)
{
    int len;
    if (!out || n <= 0) return -1;
    out[0] = 0;
    if (!valid_id(id)) return -1;
    len = snprintf(out, (size_t)n, APP_DIR "skins/%s", id);
    if (len < 0 || len >= n) { out[0] = 0; return -1; }
    return 0;
}

static int entry_index(const char *id)
{
    int i;
    if (!valid_id(id)) return -1;
    for (i = 0; i < g_count; i++) if (!strcmp(id, g_entries[i].id)) return i;
    return -1;
}

static int installed(const char *id)
{
    char root[SC_PATH], path[SC_PATH];
    if (skin_catalog_root(id, root, sizeof(root)) < 0 ||
        path_join(path, sizeof(path), root, "skin.json") < 0) return 0;
    return fs_kind(path) == 1;
}

static int fetch(const char *id, const char *file, int limit, char **out, int *len)
{
    char url[256];
    char *data;
    int n = 0, expect, rc;
    *out = NULL;
    if (id && !valid_id(id)) return fail("Invalid skin ID");
    rc = id ? snprintf(url, sizeof(url), "%s%s/%s", SKIN_CATALOG_BASE, id, file)
            : snprintf(url, sizeof(url), "%s%s", SKIN_CATALOG_BASE, file);
    if (rc < 0 || rc >= (int)sizeof(url)) return fail("Skin URL too long");
    data = (char *)malloc((size_t)limit + 2);
    if (!data) return fail("Not enough memory for skin download");
    rc = http_get(url, data, limit + 2, &n, 15000, g_error, sizeof(g_error));
    expect = http_get_expect();
    if (rc < 0 || n <= 0 || n > limit || (expect >= 0 && expect != n)) {
        free(data);
        return fail(rc < 0 ? g_error : "Skin download incomplete or too large");
    }
    data[n] = 0;
    *out = data;
    *len = n;
    return 0;
}

static int json_ok(const char *data, int n)
{
    /* json_walk strings are C strings: reject NUL escapes as well as raw NUL. */
    return !memchr(data, 0, (size_t)n) && !strstr(data, "\\u0000") &&
           json_validate(data, n) == 0;
}

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Input has already passed strict complete-document JSON validation. */
static const char *value_end(const char *p)
{
    int depth = 0, string = 0;
    do {
        char c = *p;
        if (string) {
            if (c == '\\') { p += 2; continue; }
            if (c == '"') { string = 0; if (!depth) return p + 1; }
        } else {
            if (c == '"') string = 1;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') { if (!depth) return p; if (!--depth) return p + 1; }
            else if (!depth && (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n')) return p;
        }
        p++;
    } while (*p);
    return p;
}

/* json_walk has no callbacks for empty containers. Catalog entries are flat
 * scalar records, so inspect shape before accepting their leaf callbacks. */
static int flat_object(const char *p)
{
    p = ws(p);
    if (*p++ != '{') return 0;
    p = ws(p);
    while (*p != '}') {
        if (*p != '"') return 0;
        p = ws(value_end(p));
        if (*p++ != ':') return 0;
        p = ws(p);
        if (*p == '{' || *p == '[') return 0;
        p = ws(value_end(p));
        if (*p != ',') break;
        p = ws(p + 1);
    }
    return *p == '}';
}

typedef struct { SkinCatalogEntry *entry; unsigned int seen; int bad; } EntryParse;
static void entry_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    EntryParse *p = (EntryParse *)ud;
    SkinCatalogEntry *e = p->entry;
    char *dest = NULL;
    int cap = 0, bit = 0;
    (void)num;
    if (!strcmp(path, "id")) { dest = e->id; cap = sizeof(e->id); bit = 1; }
    else if (!strcmp(path, "name")) { dest = e->name; cap = sizeof(e->name); bit = 2; }
    else if (!strcmp(path, "author")) { dest = e->author; cap = sizeof(e->author); bit = 4; }
    else if (!strcmp(path, "version")) { dest = e->version; cap = sizeof(e->version); bit = 8; }
    else if (!strcmp(path, "description")) { dest = e->description; cap = sizeof(e->description); bit = 16; }
    else if (!strcmp(path, "script")) bit = 32;
    else if (!strcmp(path, "preview")) bit = 64;
    else { p->bad = 1; return; }
    if (p->seen & (unsigned)bit) { p->bad = 1; return; }
    p->seen |= (unsigned)bit;
    if (dest) {
        if (type != JSON_STR || !str || strlen(str) >= (size_t)cap) { p->bad = 1; return; }
        memcpy(dest, str, strlen(str) + 1);
    } else {
        if (type != JSON_TRUE && type != JSON_FALSE) { p->bad = 1; return; }
        if (bit == 32) e->has_script = type == JSON_TRUE;
        else e->has_preview = type == JSON_TRUE;
    }
}

/* Structural scan prevents numeric object keys/dotted paths masquerading as an
 * array, and counts empty objects too. json_walk handles leaf decoding/types. */
static int parse_index(const char *data, int n, SkinCatalogEntry *entries, int *count)
{
    const char *p = ws(data), *key, *end;
    int schema = 0, skins = 0, total = 0;
    if (!json_ok(data, n) || *p++ != '{') return -1;
    p = ws(p);
    while (*p != '}') {
        key = p;
        if (*key != '"') return -1;
        end = value_end(key);
        p = ws(end);
        if (*p++ != ':') return -1;
        p = ws(p);
        if (end - key == 8 && !memcmp(key, "\"schema\"", 8)) {
            end = value_end(p);
            if (schema++ || end - p != 1 || *p != '1') return -1;
            p = end;
        } else if (end - key == 7 && !memcmp(key, "\"skins\"", 7)) {
            if (skins++ || *p++ != '[') return -1;
            p = ws(p);
            while (*p != ']') {
                EntryParse parse;
                int i;
                if (total >= SKIN_CATALOG_MAX || !flat_object(p)) return -1;
                end = value_end(p);
                memset(&parse, 0, sizeof(parse));
                parse.entry = &entries[total];
                if (json_walk(p, (int)(end - p), entry_cb, &parse) < 0 || parse.bad ||
                    (parse.seen & 3) != 3 || !valid_id(parse.entry->id) || !parse.entry->name[0]) return -1;
                for (i = 0; i < total; i++)
                    if (same_id(entries[i].id, parse.entry->id)) return -1;
                parse.entry->installed = installed(parse.entry->id);
                total++;
                p = ws(end);
                if (*p != ',') break;
                p = ws(p + 1);
            }
            if (*p++ != ']') return -1;
        } else return -1;
        p = ws(p);
        if (*p != ',') break;
        p = ws(p + 1);
    }
    if (*p != '}' || schema != 1 || skins != 1) return -1;
    *count = total;
    return 0;
}

typedef struct { EntryParse fields; int schema, schema_seen; } LocalParse;
static void local_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    LocalParse *p = (LocalParse *)ud;
    if (!strcmp(path, "schema")) {
        if (p->schema_seen++ || type != JSON_NUM || num != 1) p->fields.bad = 1;
        else p->schema = 1;
    } else if (!strcmp(path, "script") && type == JSON_STR) {
        if (p->fields.seen & 32 || !str || strcmp(str, "skin.lua")) p->fields.bad = 1;
        p->fields.seen |= 32;
        p->fields.entry->has_script = 1;
    } else if (!strcmp(path, "id") || !strcmp(path, "name") ||
               !strcmp(path, "author") || !strcmp(path, "version") ||
               !strcmp(path, "description") || !strcmp(path, "script") ||
               !strcmp(path, "preview")) {
        entry_cb(&p->fields, path, type, str, num);
    }
    /* Runtime owns colors/page flags. Discovery never loads or executes Lua. */
}

static char *read_json(const char *path, int *len)
{
    char *data = (char *)malloc(SKIN_CATALOG_JSON_MAX + 2);
    int n = 0;
    if (!data) return NULL;
#ifdef SKIN_HOST_TEST
    {
        FILE *f = fopen(path, "rb");
        if (!f) { free(data); return NULL; }
        n = (int)fread(data, 1, SKIN_CATALOG_JSON_MAX + 1, f);
        if (ferror(f)) n = -1;
        fclose(f);
    }
#else
    {
        int fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd < 0) { free(data); return NULL; }
        while (n < SKIN_CATALOG_JSON_MAX + 1) {
            int r = sceIoRead(fd, data + n, SKIN_CATALOG_JSON_MAX + 1 - n);
            if (r < 0) { n = -1; break; }
            if (!r) break;
            n += r;
        }
        sceIoClose(fd);
    }
#endif
    if (n <= 0 || n > SKIN_CATALOG_JSON_MAX) { free(data); return NULL; }
    data[n] = 0;
    *len = n;
    return data;
}

int skin_catalog_local_version(const char*id,char*out,int cap)
{
    char root[SC_PATH],path[SC_PATH];
    int n=0,valid;
    char *data;
    LocalParse p;
    SkinCatalogEntry entry;
    if(!out||cap<1)return -1;
    out[0]=0;
    if(skin_catalog_root(id,root,sizeof(root))<0||path_join(path,sizeof(path),root,"skin.json")<0)return -1;
    data=read_json(path,&n);
    if(!data)return -1;
    memset(&p,0,sizeof(p));
    memset(&entry,0,sizeof(entry));
    p.fields.entry=&entry;
    valid=json_ok(data,n)&&json_walk(data,n,local_cb,&p)>=0&&!p.fields.bad&&p.schema==1&&same_id(entry.id,id);
    free(data);
    if(!valid)return -1;
    snprintf(out,cap,"%s",entry.version[0]?entry.version:"未知");
    return 0;
}
static void local_entry(const char *id, SkinCatalogEntry *entries, int *count)
{
    char root[SC_PATH], path[SC_PATH], old[SC_PATH];
    char *data;
    int n, i;
    LocalParse p;
    SkinCatalogEntry entry;
    if (*count >= SKIN_CATALOG_MAX || !valid_id(id) ||
        skin_catalog_root(id, root, sizeof(root)) < 0) return;
    /* Recover a publish interrupted after root -> .old, before root -> .new. */
    if (snprintf(old, sizeof(old), "%s.old", root) >= (int)sizeof(old)) return;
    if (!fs_kind(root) && fs_kind(old)) fs_rename(old, root);
    if (fs_kind(root) != 2 || path_join(path, sizeof(path), root, "skin.json") < 0) return;
    data = read_json(path, &n);
    if (!data) return;
    memset(&p, 0, sizeof(p));
    memset(&entry, 0, sizeof(entry));
    p.fields.entry = &entry;
    if (!json_ok(data, n) || *ws(data) != '{' ||
        json_walk(data, n, local_cb, &p) < 0 || p.fields.bad || p.schema != 1 ||
        (p.fields.seen & 3) != 3 || !same_id(entry.id, id) || !entry.name[0]) {
        free(data); return;
    }
    free(data);
    if (entry.has_script) {
        if (path_join(path, sizeof(path), root, "skin.lua") < 0 || fs_kind(path) != 1) return;
    }
    if (path_join(path, sizeof(path), root, "preview.png") < 0) return;
    if (fs_kind(path) == 1) entry.has_preview = 1;
    entry.installed = 1;
    for (i = 0; i < *count; i++) if (same_id(entries[i].id, id)) return;
    entries[(*count)++] = entry;
}

/* A .old directory carries the safe ID needed to recover after a stopped swap. */
static void local_child(const char *name, SkinCatalogEntry *entries, int *count)
{
    size_t n = strlen(name);
    char id[33];
    if (n > 4 && n <= 36 && !strcmp(name + n - 4, ".old")) {
        memcpy(id, name, n - 4); id[n - 4] = 0;
        local_entry(id, entries, count);
    } else local_entry(name, entries, count);
}

static int scan_local(SkinCatalogEntry *entries)
{
    int count = 0;
#ifdef SKIN_HOST_TEST
    DIR *dir = opendir(APP_DIR "skins");
    struct dirent *de;
    if (!dir) return 0;
    while (count < SKIN_CATALOG_MAX && (de = readdir(dir)) != NULL)
        local_child(de->d_name, entries, &count);
    closedir(dir);
#else
    int dir = sceIoDopen(APP_DIR "skins");
    SceIoDirent de;
    if (dir < 0) return 0;
    while (count < SKIN_CATALOG_MAX) {
        int rc;
        memset(&de, 0, sizeof(de));
        rc = sceIoDread(dir, &de);
        if (rc <= 0) break;
        local_child(de.d_name, entries, &count);
    }
    sceIoDclose(dir);
#endif
    return count;
}

/* Installed local records have priority at capacity; remote metadata replaces
 * matching IDs and remaining remote records fill the available slots. */
static int merge_entries(SkinCatalogEntry *local, int local_count,
                         const SkinCatalogEntry *remote, int remote_count)
{
    int i, j, metro = 0;
    for (i = 0; i < local_count; i++)
        if (same_id(local[i].id, "metro") && local[i].installed) metro = 1;
    /* Metro now owns its day/night preference. Keep legacy files for runtime
     * migration, but do not advertise a second entry from disk or old indexes. */
    if (metro) {
        for (i = j = 0; i < local_count; i++)
            if (!same_id(local[i].id, "metro_day")) local[j++] = local[i];
        local_count = j;
    }
    for (i = 0; i < remote_count; i++) {
        if (metro && same_id(remote[i].id, "metro_day")) continue;
        for (j = 0; j < local_count; j++) if (same_id(local[j].id, remote[i].id)) break;
        if (j < local_count) {
            int present = local[j].installed;
            local[j] = remote[i];
            local[j].installed = present;
        } else if (local_count < SKIN_CATALOG_MAX) local[local_count++] = remote[i];
    }
    return local_count;
}

int skin_catalog_load_local(void)
{
    SkinCatalogEntry *candidate;
    int count, i;
    sc_bind();
    begin(SKIN_CATALOG_REFRESHING);
    candidate = (SkinCatalogEntry *)calloc(SKIN_CATALOG_MAX, sizeof(*candidate));
    if (!candidate) return fail("Not enough memory for local skins");
    count = scan_local(candidate);
    for (i = 0; i < g_count; i++) g_entries[i].installed = installed(g_entries[i].id);
    count = merge_entries(candidate, count, g_entries, g_count);
    memcpy(g_entries, candidate, sizeof(g_entries));
    g_count = count;
    free(candidate);
    return done();
}

int skin_catalog_refresh(void)
{
    SkinCatalogEntry *candidate;
    char *data;
    int n, count, rc;
    sc_bind();
    begin(SKIN_CATALOG_REFRESHING);
    if (fetch(NULL, "index.json", SKIN_CATALOG_JSON_MAX, &data, &n) < 0) return -1;
    candidate = (SkinCatalogEntry *)calloc(SKIN_CATALOG_MAX, sizeof(*candidate));
    if (!candidate) { free(data); return fail("Not enough memory for skin catalog"); }
    rc = parse_index(data, n, candidate, &count);
    free(data);
    if (rc < 0) { free(candidate); return fail("Invalid skin catalog index"); }
    memset(g_entries, 0, sizeof(g_entries));
    g_count = scan_local(g_entries);
    g_count = merge_entries(g_entries, g_count, candidate, count);
    free(candidate);
    return done();
}

#define SC_ASSET_MAX 20
#define SC_ASSET_TOTAL (1024 * 1024)
typedef struct { char id[33], file[44], type[6]; unsigned seen; } CatalogAsset;
typedef struct { CatalogAsset items[SC_ASSET_MAX]; int count; } CatalogAssets;

typedef struct { CatalogAsset *asset; int bad; } AssetParse;
static void asset_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    AssetParse *p = (AssetParse *)ud;
    CatalogAsset *a = p->asset;
    char *dst;
    int cap, bit;
    (void)num;
    if (!strcmp(path, "id")) { dst = a->id; cap = sizeof(a->id); bit = 1; }
    else if (!strcmp(path, "file")) { dst = a->file; cap = sizeof(a->file); bit = 2; }
    else if (!strcmp(path, "type")) { dst = a->type; cap = sizeof(a->type); bit = 4; }
    else { p->bad = 1; return; }
    if (type != JSON_STR || !str || strlen(str) >= (size_t)cap || (a->seen & (unsigned)bit)) {
        p->bad = 1; return;
    }
    a->seen |= (unsigned)bit;
    memcpy(dst, str, strlen(str) + 1);
}

/* Inspect real array/object structure instead of json_walk's ambiguous paths. */
static int parse_assets(const char *data, CatalogAssets *assets)
{
    const char *p = ws(data) + 1, *key, *end;
    int seen = 0, images = 0, sounds = 0;
    memset(assets, 0, sizeof(*assets));
    p = ws(p);
    while (*p != '}') {
        key = p;
        end = value_end(key);
        p = ws(end);
        if (*p++ != ':') return -1;
        p = ws(p);
        /* Escaped root key spellings are rejected: they must not conceal assets. */
        if (memchr(key, '\\', (size_t)(end - key))) return -1;
        if (end - key == 8 && !memcmp(key, "\"assets\"", 8)) {
            if (seen++ || *p++ != '[') return -1;
            p = ws(p);
            while (*p != ']') {
                AssetParse parse;
                CatalogAsset *a;
                size_t len;
                int i, image;
                if (assets->count >= SC_ASSET_MAX || !flat_object(p)) return -1;
                end = value_end(p);
                a = &assets->items[assets->count];
                memset(&parse, 0, sizeof(parse)); parse.asset = a;
                if (json_walk(p, (int)(end - p), asset_cb, &parse) < 0 || parse.bad ||
                    a->seen != 7 || !valid_id(a->id) || strncmp(a->file, "assets/", 7) ||
                    !asset_name(a->file + 7)) return -1;
                image = !strcmp(a->type, "image");
                len = strlen(a->file);
                if (image) { if (++images > 12 || strcmp(a->file + len - 4, ".png")) return -1; }
                else if (strcmp(a->type, "sound") || ++sounds > 8 || strcmp(a->file + len - 4, ".wav")) return -1;
                for (i = 0; i < assets->count; i++)
                    if (same_id(a->id, assets->items[i].id) || same_id(a->file, assets->items[i].file)) return -1;
                assets->count++;
                p = ws(end);
                if (*p != ',') break;
                p = ws(p + 1);
            }
            if (*p++ != ']') return -1;
        } else p = value_end(p);
        p = ws(p);
        if (*p != ',') break;
        p = ws(p + 1);
    }
    return *p == '}' ? 0 : -1;
}

typedef struct { const SkinCatalogEntry *entry; int id_seen, script_seen, bad; } ManifestParse;
static void manifest_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    ManifestParse *p = (ManifestParse *)ud;
    (void)num;
    if (!strcmp(path, "id")) {
        if (p->id_seen++ || type != JSON_STR || !str || strcmp(str, p->entry->id)) p->bad = 1;
    } else if (!strcmp(path, "script")) {
        int enabled = type == JSON_TRUE || (type == JSON_STR && str && !strcmp(str, "skin.lua"));
        if (p->script_seen++ || (type != JSON_TRUE && type != JSON_FALSE &&
            !(type == JSON_STR && str && !strcmp(str, "skin.lua"))) ||
            enabled != p->entry->has_script) p->bad = 1;
    }
}

static int manifest_ok(const char *data, int n, const SkinCatalogEntry *entry)
{
    ManifestParse p;
    memset(&p, 0, sizeof(p));
    p.entry = entry;
    if (!json_ok(data, n) || *ws(data) != '{' ||
        json_walk(data, n, manifest_cb, &p) < 0 || p.bad || p.id_seen != 1) return 0;
    /* Omission means no script. Required scripts must be declared consistently. */
    return !entry->has_script || p.script_seen == 1;
}

static int preview_cache_path(const char *id, char *out, int n, const char *suffix)
{
    int len;
    if (!out || n <= 0) return -1;
    out[0] = 0;
    if (!valid_id(id)) return -1;
    len = snprintf(out, (size_t)n, APP_DIR "skins/.previews/%s.png%s", id, suffix);
    if (len < 0 || len >= n) { out[0] = 0; return -1; }
    return 0;
}

int skin_catalog_preview_path(const char *id, char *out, int n)
{
    char root[SC_PATH];
    if (preview_cache_path(id, out, n, "") < 0) return -1;
    if (fs_kind(out) == 1) return 0;
    if (skin_catalog_root(id, root, sizeof(root)) == 0 &&
        path_join(out, n, root, "preview.png") == 0 && fs_kind(out) == 1) return 0;
    out[0] = 0;
    return -1;
}

static unsigned long png_u32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

static unsigned long wav_u32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned wav_u16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

static int image_ok(const unsigned char *p, int n)
{
    static const unsigned char sig[] = {137,80,78,71,13,10,26,10};
    unsigned long w, h;
    if (n < 33 || memcmp(p, sig, 8) || png_u32(p + 8) != 13 || memcmp(p + 12, "IHDR", 4)) return 0;
    w = png_u32(p + 16); h = png_u32(p + 20);
    return w > 0 && h > 0 && w <= 512 && h <= 512;
}

static int sound_ok(const unsigned char *p, int n)
{
    unsigned long rate = 0, bytes = 0, pos = 12;
    unsigned channels = 0, block = 0;
    int fmt = 0, data = 0;
    if (n < 44 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4) ||
        wav_u32(p + 4) != (unsigned long)n - 8) return 0;
    while (pos + 8 <= (unsigned long)n) {
        unsigned long size = wav_u32(p + pos + 4), next;
        const unsigned char *q = p + pos + 8;
        if (size > (unsigned long)n - pos - 8) return 0;
        next = pos + 8 + size + (size & 1);
        if (next > (unsigned long)n) return 0;
        if (!memcmp(p + pos, "fmt ", 4)) {
            if (fmt++ || size < 16 || wav_u16(q) != 1 || wav_u16(q + 14) != 16) return 0;
            channels = wav_u16(q + 2); rate = wav_u32(q + 4); block = wav_u16(q + 12);
            if (channels != 1 || rate != 44100 ||
                block != channels * 2 || wav_u32(q + 8) != rate * block) return 0;
        } else if (!memcmp(p + pos, "data", 4)) {
            if (data++) return 0;
            bytes = size;
        }
        pos = next;
    }
    return pos == (unsigned long)n && fmt == 1 && data == 1 && bytes > 0 &&
           bytes % block == 0 && bytes * 1000 <= rate * block * 350;
}

static int install_assets(const char *id, const char *staging, const CatalogAssets *assets)
{
    char path[SC_PATH];
    int i, total = 0;
    if (!assets->count) return 0;
    if (path_join(path, sizeof(path), staging, "assets") < 0 || fs_mkdir(path) < 0)
        return fail("Cannot create skin assets directory");
    for (i = 0; i < assets->count; i++) {
        const CatalogAsset *a = &assets->items[i];
        char *data = NULL;
        int n, image = !strcmp(a->type, "image");
        if (fetch(id, a->file, image ? SKIN_CATALOG_PREVIEW_MAX : 64 * 1024, &data, &n) < 0) return -1;
        total += n;
        if (total > SC_ASSET_TOTAL || !(image ? image_ok((unsigned char *)data, n) : sound_ok((unsigned char *)data, n))) {
            free(data); return fail("Invalid skin image/sound or assets exceed 1MB");
        }
        if (path_join(path, sizeof(path), staging, a->file) < 0 || write_file(path, data, n) < 0) {
            free(data); return fail("Cannot write skin resource");
        }
        free(data);
        g_progress = 45 + 20 * (i + 1) / assets->count;
    }
    return 0;
}

static int preview_fetch(const char *id)
{
    static const unsigned char sig[] = {137,80,78,71,13,10,26,10};
    char *data, path[SC_PATH], temp[SC_PATH], old[SC_PATH];
    unsigned long w, h;
    int n;
    if (fetch(id, "preview.png", SKIN_CATALOG_PREVIEW_MAX, &data, &n) < 0) return -1;
    if (n < 33 || memcmp(data, sig, 8) || png_u32((unsigned char *)data + 8) != 13 ||
        memcmp(data + 12, "IHDR", 4)) { free(data); return fail("Invalid skin preview PNG"); }
    w = png_u32((unsigned char *)data + 16);
    h = png_u32((unsigned char *)data + 20);
    if (!w || !h || w > 512 || h > 512) { free(data); return fail("Skin preview dimensions exceed 512"); }
    if (preview_cache_path(id, path, sizeof(path), "") < 0 ||
        preview_cache_path(id, temp, sizeof(temp), ".new") < 0 ||
        preview_cache_path(id, old, sizeof(old), ".old") < 0 ||
        fs_mkdir(APP_DIR "skins") < 0 || fs_mkdir(APP_DIR "skins/.previews") < 0) {
        free(data); return fail("Cannot create skin preview cache");
    }
    if (!fs_kind(path) && fs_kind(old) && fs_rename(old, path) < 0) {
        free(data); return fail("Cannot recover skin preview");
    }
    if (fs_kind(temp) && fs_remove(temp) < 0) { free(data); return fail("Cannot clear skin preview staging"); }
    if (write_file(temp, data, n) < 0) { free(data); fs_remove(temp); return fail("Cannot write skin preview"); }
    free(data);
    if (fs_kind(old) && fs_remove(old) < 0) { fs_remove(temp); return fail("Cannot clear old skin preview"); }
    if (fs_kind(path) && fs_rename(path, old) < 0) { fs_remove(temp); return fail("Cannot back up skin preview"); }
    if (fs_rename(temp, path) < 0) {
        if (fs_kind(old)) fs_rename(old, path);
        fs_remove(temp);
        return fail("Cannot publish skin preview");
    }
    fs_remove(old);
    return 0;
}

int skin_catalog_fetch_preview(const char *id)
{
    int i = entry_index(id);
    sc_bind();
    begin(SKIN_CATALOG_FETCHING_PREVIEW);
    if (i < 0 || !g_entries[i].has_preview) return fail("Skin preview is not listed");
    if (preview_fetch(id) < 0) return -1;
    return done();
}

static int version_older(const char*remote,const char*local)
{
    const char *a=remote,*b=local;
    if(!a[0]||!b[0])return 0;
    for(int group=0;group<6;++group){
        unsigned long av=0,bv=0;
        if((*a&&(*a<'0'||*a>'9'))||(*b&&(*b<'0'||*b>'9')))return 0;
        while(*a>='0'&&*a<='9'){if(av>1000000)return 0;av=av*10+*a++-'0';}
        while(*b>='0'&&*b<='9'){if(bv>1000000)return 0;bv=bv*10+*b++-'0';}
        if(av!=bv)return av<bv;
        if(!*a&&!*b)return 0;
        if(*a=='.')++a;
        if(*b=='.')++b;
    }
    return 0;
}

int skin_catalog_install(const char *id)
{
    char root[SC_PATH], staging[SC_PATH], old[SC_PATH], path[SC_PATH];
    char *data = NULL;
    CatalogAssets assets;
    int n, i = entry_index(id), had_old;
    sc_bind();
    begin(SKIN_CATALOG_INSTALLING);
    SC_LOG("skin install: start id=%s", id ? id : "-");
    if (i < 0) return fail("Skin ID is not in the catalog");
    if (skin_catalog_root(id, root, sizeof(root)) < 0 ||
        snprintf(staging, sizeof(staging), "%s.new", root) >= (int)sizeof(staging) ||
        snprintf(old, sizeof(old), "%s.old", root) >= (int)sizeof(old)) return fail("Skin path too long");
    if (fs_mkdir(APP_DIR "skins") < 0) return fail("Cannot create skin directory");
    /* A stopped swap is restored BEFORE any network/validation failure can occur. */
    if (!fs_kind(root) && fs_kind(old) && fs_rename(old, root) < 0) return fail("Cannot recover previous skin");
    g_entries[i].installed = installed(id);
    {char local_version[24];if(g_entries[i].installed&&skin_catalog_local_version(id,local_version,sizeof(local_version))==0&&version_older(g_entries[i].version,local_version))return fail("Remote skin older than local; kept installed version");}
    if (clear_skin_dir(staging) < 0 || fs_mkdir(staging) < 0) return fail("Cannot prepare skin staging");
    if (fetch(id, "skin.json", SKIN_CATALOG_JSON_MAX, &data, &n) < 0) goto failed;
    if (!manifest_ok(data, n, &g_entries[i])) { fail("Skin manifest ID/script differs from catalog"); goto failed; }
    if (parse_assets(data, &assets) < 0) { fail("Invalid skin asset manifest"); goto failed; }
    if (path_join(path, sizeof(path), staging, "skin.json") < 0 || write_file(path, data, n) < 0) {
        fail("Cannot write skin manifest"); goto failed;
    }
    free(data); data = NULL;
    g_progress = 35;
    if (g_entries[i].has_script) {
        if (fetch(id, "skin.lua", SKIN_CATALOG_SCRIPT_MAX, &data, &n) < 0) goto failed;
        if (memchr(data, 0, (size_t)n)) { fail("Skin script must be text Lua"); goto failed; }
        if (path_join(path, sizeof(path), staging, "skin.lua") < 0 || write_file(path, data, n) < 0) {
            fail("Cannot write skin script"); goto failed;
        }
        free(data); data = NULL;
    }
    if (install_assets(id, staging, &assets) < 0) goto failed;
    g_progress = 65;
    if (skin_runtime_validate(staging, g_error, sizeof(g_error)) < 0) { fail(g_error); goto failed; }
    /* Do not touch the installed version until all required files validate. */
    if (clear_skin_dir(old) < 0) { fail("Cannot clear previous skin backup"); goto failed; }
    had_old = fs_kind(root) != 0;
    if (had_old && fs_rename(root, old) < 0) { fail("Cannot back up installed skin"); goto failed; }
    if (fs_rename(staging, root) < 0) {
        if (had_old && fs_rename(old, root) < 0)
            fail("Skin publish failed; previous skin retained in .old");
        else fail("Cannot publish installed skin");
        goto failed;
    }
    /* An undeletable backup remains available for recovery; install is complete. */
    clear_skin_dir(old);
    g_entries[i].installed = 1;
    g_progress = 85;
    if (g_entries[i].has_preview) {
        preview_fetch(id); /* Optional: the required skin is already committed. */
        g_error[0] = 0;
    }
    SC_LOG("skin install: ok id=%s", g_entries[i].id);
    return done();
failed:
    free(data);
    clear_skin_dir(staging);
    g_entries[i].installed = installed(id);
    SC_LOG("skin install: failed id=%s: %s", id ? id : "-", g_error);
    return -1;
}

int skin_catalog_install_id(const char *id) { return skin_catalog_install(id); }
const SkinCatalogEntry *skin_catalog_items(void) { return g_entries; }
int skin_catalog_count(void) { return g_count; }
int skin_catalog_state(void) { return g_state; }
int skin_catalog_progress(void) { return g_progress; }
const char *skin_catalog_install_error(void) { return g_install_error; }
const char *skin_catalog_error(void) { return g_error; }
