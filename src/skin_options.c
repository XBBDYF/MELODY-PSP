/* Active-skin preferences: bounded metadata, private per-skin atomic saves. */
#include "skin_options.h"
#include "json.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#ifdef SKIN_HOST_TEST
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#ifndef APP_DIR
#define APP_DIR "./"
#endif
#else
#include "app.h"
#include <pspiofilemgr.h>
#endif

#define META_LIMIT (24 * 1024)
#define PATH_CAP 256
static SkinOption entries[SKIN_OPTIONS_MAX];
static int count;
static char active_id[33];

typedef struct {
    SkinOption option;
    unsigned fields, labels;
    int bad;
    const char *start, *end;
} Draft;

static const char *ws(const char *p) { while (*p && strchr(" \r\n\t", *p)) ++p; return p; }
/* Called only after strict complete-document validation. */
static const char *skip(const char *p)
{
    char close;
    p = ws(p);
    if (*p == '"') {
        for (++p; *p; ++p) { if (*p == '\\') ++p; else if (*p == '"') return p + 1; }
        return p;
    }
    if (*p == '{' || *p == '[') {
        close = *p++ == '{' ? '}' : ']';
        while (*(p = ws(p)) && *p != close) {
            if (*p == ',' || *p == ':') ++p; else p = skip(p);
        }
        return *p ? p + 1 : p;
    }
    while (*p && !strchr(",]} \r\n\t", *p)) ++p;
    return p;
}
static int eq(const char *p, int n, const char *s) { return n == (int)strlen(s) && !memcmp(p, s, (size_t)n); }
/* Decode keys through the real parser too, including JSON escape sequences. */
static void key_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    (void)path; (void)num;
    if (type == JSON_STR && strlen(str) < 64) strcpy((char *)ud, str);
}
static const char *field(const char *object, const char *name)
{
    const char *p = ws(object), *end;
    char key[64];
    if (*p++ != '{') return NULL;
    while (*(p = ws(p)) && *p != '}') {
        end = skip(p); key[0] = 0;
        json_walk(p, (int)(end - p), key_cb, key);
        p = ws(end); if (*p++ != ':') return NULL;
        p = ws(p);
        if (!strcmp(key, name)) return p;
        p = ws(skip(p)); if (*p == ',') ++p; else break;
    }
    return NULL;
}
static int valid_id(const char *s)
{
    int n;
    if (!s || !*s) return 0;
    for (n = 0; s[n]; ++n) {
        unsigned char c = (unsigned char)s[n];
        if (n >= 32 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}
static int integer(const char *p, int *value)
{
    long v; char *end;
    if (!p) return 0;
    errno = 0; v = strtol(p, &end, 10);
    if (p == end || errno || v < INT_MIN || v > INT_MAX ||
        (*end && !strchr(",]} \r\n\t", *end))) return 0;
    *value = (int)v; return 1;
}
static void option_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    Draft *drafts = (Draft *)ud, *d;
    const char *seg[5]; int len[5], n, idx = 0, i, value; SkinOption *o;
    (void)num;
    n = json_split_path(path, seg, len, 5);
    if (n < 3 || !eq(seg[0], len[0], "preferences")) return;
    if (len[1] < 1 || len[1] > 2) return;
    for (i = 0; i < len[1]; ++i) { if (seg[1][i] < '0' || seg[1][i] > '9') return; idx = idx * 10 + seg[1][i] - '0'; }
    if (idx >= SKIN_OPTIONS_MAX) return;
    d = &drafts[idx]; o = &d->option;
    if (!d->start) return;
    if (n == 4 && eq(seg[2], len[2], "options")) {
        if (len[3] != 1 || seg[3][0] < '0' || seg[3][0] > '7' || type != JSON_STR || !str || !*str || strlen(str) > 48) { d->bad = 1; return; }
        i = seg[3][0] - '0'; strcpy(o->options[i], str); d->labels |= 1u << i; return;
    }
    if (n != 3) return;
    if (eq(seg[2], len[2], "id") || eq(seg[2], len[2], "label") || eq(seg[2], len[2], "type")) {
        if (type != JSON_STR) { d->bad = 1; return; }
        if (eq(seg[2], len[2], "id")) { if (!valid_id(str)) d->bad = 1; else { strcpy(o->id, str); d->fields |= 1; } }
        else if (eq(seg[2], len[2], "label")) { if (!*str || strlen(str) > 63) d->bad = 1; else { strcpy(o->label, str); d->fields |= 2; } }
        else { o->type = !strcmp(str, "toggle") ? 0 : !strcmp(str, "choice") ? 1 : !strcmp(str, "range") ? 2 : -1; d->fields |= 4; }
    } else {
        const char *name = NULL; int *target = NULL;
        if (eq(seg[2], len[2], "default")) { name = "default"; target = &o->default_value; }
        if (eq(seg[2], len[2], "min")) { name = "min"; target = &o->min; }
        if (eq(seg[2], len[2], "max")) { name = "max"; target = &o->max; }
        if (eq(seg[2], len[2], "step")) { name = "step"; target = &o->step; }
        if (target) { if (type != JSON_NUM || !integer(field(d->start, name), &value)) d->bad = 1; else *target = value; }
    }
}
static int normalized(const SkinOption *o, int v)
{
    if (v < o->min) v = o->min;
    if (v > o->max) v = o->max;
    return o->min + ((v - o->min) / o->step) * o->step;
}
void skin_options_reset(void) { memset(entries, 0, sizeof(entries)); count = 0; active_id[0] = 0; }
int skin_options_count(void) { return count; }
const SkinOption *skin_options_at(int index) { return index >= 0 && index < count ? &entries[index] : NULL; }
int skin_options_value(const char *id, int fallback)
{
    int i; if (id) for (i = 0; i < count; ++i) if (!strcmp(entries[i].id, id)) return entries[i].value;
    return fallback;
}
static int pref_path(char *out, const char *suffix)
{
    int n = snprintf(out, PATH_CAP, APP_DIR "skin-prefs/%s.txt%s", active_id, suffix);
    return n >= 0 && n < PATH_CAP ? 0 : -1;
}
static int read_bounded(const char *path, char *out, int cap)
{
    FILE *f; size_t n; int good;
    io_lock(); f = fopen(path, "rb");
    if (!f) { io_unlock(); return -1; }
    n = fread(out, 1, (size_t)cap, f); good = !ferror(f) && n < (size_t)cap;
    if (fclose(f)) good = 0;
    io_unlock(); if (!good) return -1; out[n] = 0; return (int)n;
}
void skin_options_load(const char *root, const char *id)
{
    char text[META_LIMIT + 1], path[PATH_CAP]; Draft drafts[SKIN_OPTIONS_MAX];
    const char *p, *q; int n, i, j, total = 0;
    skin_options_reset();
    if (!root || !*root || strstr(root, "..") || !valid_id(id)) return;
    n = snprintf(path, sizeof(path), "%s/skin.json", root);
    if (n < 0 || n >= (int)sizeof(path)) return;
    n = read_bounded(path, text, sizeof(text));
    if (n < 0 || json_validate(text, n) < 0 || *ws(text) != '{') return;
    p = field(text, "preferences"); if (!p || *p != '[') return;
    memset(drafts, 0, sizeof(drafts)); ++p;
    while (*(p = ws(p)) != ']') {
        if (total >= SKIN_OPTIONS_MAX) return;
        q = skip(p); drafts[total].start = p; drafts[total].end = q;
        drafts[total].bad = *p != '{'; drafts[total].option.max = 100; drafts[total].option.step = 1;
        ++total; p = ws(q); if (*p == ',') ++p; else break;
    }
    if (json_walk(text, n, option_cb, drafts) < 0) return;
    for (i = 0; i < total; ++i) {
        Draft *d = &drafts[i]; SkinOption *o = &d->option; const char *v;
        if (d->bad || d->fields != 7 || o->type < 0) continue;
        /* Required scalar shapes must not be hidden containers with no leaves. */
        if (!(v = field(d->start, "id")) || *v != '"' || !(v = field(d->start, "label")) || *v != '"' || !(v = field(d->start, "type")) || *v != '"') continue;
        { const char *names[] = { "default", "min", "max", "step" }; int k, unused;
          for (k = 0; k < 4; ++k) if ((v = field(d->start, names[k])) && !integer(v, &unused)) break;
          if (k != 4) continue; }
        if (o->type == SKIN_OPTION_CHOICE) {
            v = field(d->start, "options"); if (!v || *v != '[') continue;
            j = 0; ++v; while (*(v = ws(v)) != ']') { ++j; v = ws(skip(v)); if (*v == ',') ++v; else break; }
            if (j < 2 || j > 8 || d->labels != (1u << j) - 1) continue;
            o->option_count = j; o->min = 0; o->max = j - 1; o->step = 1;
        } else if (o->type == SKIN_OPTION_TOGGLE) { o->min = 0; o->max = 1; o->step = 1; }
        else if (o->min < -1000 || o->max > 1000 || o->min > o->max || o->step < 1 || o->step > 100) continue;
        for (j = 0; j < count; ++j) if (!strcmp(entries[j].id, o->id)) break;
        if (j < count) { mlog("skin options: duplicate id %s (first retained)", o->id); continue; }
        o->value = o->default_value = normalized(o, o->default_value); entries[count++] = *o;
    }
    strcpy(active_id, id);
    if (pref_path(path, "") < 0) return;
    n = read_bounded(path, text, 1537);
    if (n < 0) { if (pref_path(path, ".old") < 0 || (n = read_bounded(path, text, 1537)) < 0) return; }
    p = text;
    while (*p) {
        char key[33], number[64]; size_t len; int value;
        q = strchr(p, '\n'); if (!q) q = p + strlen(p);
        len = (size_t)(q - p);
        if (len < 128) {
            const char *equal = memchr(p, '=', len);
            if (equal && equal > p && equal - p <= 32 && q - equal - 1 < (int)sizeof(number)) {
                memcpy(key, p, (size_t)(equal-p)); key[equal-p] = 0;
                memcpy(number, equal+1, (size_t)(q-equal-1)); number[q-equal-1] = 0;
                if (valid_id(key) && integer(number, &value) && *ws(skip(number)) == 0)
                    for (j = 0; j < count; ++j) if (!strcmp(key, entries[j].id)) entries[j].value = normalized(&entries[j], value);
            }
        }
        p = *q ? q + 1 : q;
    }
}
static int save(void)
{
    char path[PATH_CAP], tmp[PATH_CAP], old[PATH_CAP]; FILE *f; int i, ok = 1, had = 0;
    if (!active_id[0] || pref_path(path, "") || pref_path(tmp, ".tmp") || pref_path(old, ".old")) return -1;
    io_lock();
#ifdef SKIN_HOST_TEST
#ifdef _WIN32
    _mkdir(APP_DIR "skin-prefs");
#else
    mkdir(APP_DIR "skin-prefs", 0777);
#endif
#else
    sceIoMkdir(APP_DIR "skin-prefs", 0777);
#endif
    f = fopen(tmp, "wb"); if (!f) { io_unlock(); return -1; }
    for (i = 0; i < count; ++i) if (fprintf(f, "%s=%d\n", entries[i].id, entries[i].value) < 0) ok = 0;
    if (fflush(f)) ok = 0;
    if (fclose(f)) ok = 0;
    if (!ok) { remove(tmp); io_unlock(); return -1; }
    f = fopen(path, "rb"); if (f) { had = 1; fclose(f); }
    if (had) {
        /* Never overwrite the only surviving backup if its removal fails. */
        if (remove(old) && errno != ENOENT) ok = 0;
        if (ok && rename(path, old)) ok = 0;
    }
    if (ok && rename(tmp, path)) { if (had) rename(old, path); ok = 0; }
    if (!ok) remove(tmp);
    io_unlock(); return ok ? 0 : -1;
}
int skin_options_set(int index, int value)
{
    SkinOption *o; int previous;
    if (index < 0 || index >= count) return -1;
    o = &entries[index];
    if (value < o->min || value > o->max || (value-o->min) % o->step) return -1;
    if (value == o->value) return 0;
    previous = o->value; o->value = value;
    if (save()) { o->value = previous; return -2; }
    return 0;
}
int skin_options_adjust(int index, int direction)
{
    SkinOption *o; int v;
    if (index < 0 || index >= count || !direction) return -1;
    o = &entries[index]; v = o->value + (direction > 0 ? o->step : -o->step);
    if (o->type == SKIN_OPTION_TOGGLE) v = 1-o->value;
    else if (o->type == SKIN_OPTION_CHOICE) { if (v > o->max) v = 0; if (v < 0) v = o->max; }
    else v = normalized(o, v);
    return skin_options_set(index, v);
}
