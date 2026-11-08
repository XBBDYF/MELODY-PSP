#include "skin_sound.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __PSP__
#include <pspkernel.h>
#include "log.h"
#define SOUND_LOG(...) mlog(__VA_ARGS__)
static int g_lock = -1;
static int lock_sound(int wait)
{
    SceUInt timeout = 5000;
    if (g_lock < 0) return 0;
    return (wait ? sceKernelWaitSema(g_lock, 1, &timeout) :
                   sceKernelPollSema(g_lock, 1)) >= 0;
}
static void unlock_sound(void) { sceKernelSignalSema(g_lock, 1); }
#else
#define SOUND_LOG(...) ((void)0)
/* Host tests are single-threaded; PSP uses the actual kernel semaphore. */
static int lock_sound(int wait) { (void)wait; return 1; }
static void unlock_sound(void) { }
#endif

#define MAX_SOUNDS 8
#define CACHE_BYTES 196608u
#define MAX_FRAMES 15435u
#define MANIFEST_BYTES 32768
struct Sound { char id[64]; short *pcm; unsigned frames; };
static struct Sound g_sounds[MAX_SOUNDS];
static volatile int g_count;
static volatile unsigned g_plays, g_mixed;
static volatile int g_peak;
static const char *volatile g_error = "not loaded";
static unsigned g_bytes, g_pos;
static volatile int g_active;
static int g_voice = -1;

static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

/* Never hold a whole WAV alongside its PCM allocation. Validate all chunks
 * against physical and declared RIFF bounds before allocating raw PCM. */
static short *load_wav(const char *path, unsigned *frames)
{
    FILE *f = fopen(path, "rb");
    unsigned char h[16];
    long size;
    unsigned end, off = 12, data_off = 0, data_size = 0;
    int fmt = 0, bad = 0;
    short *pcm = NULL;
    if (!f) { g_error = "wav open failed"; return NULL; }
    g_error = "wav format/budget invalid";
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 44 || size > 65536 ||
        fseek(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12 ||
        memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto done;
    end = le32(h + 4);
    if (end < 36 || end != (unsigned)size - 8) goto done;
    end += 8;
    while (off < end) {
        unsigned n, padded;
        if (end - off < 8 || fseek(f, off, SEEK_SET) || fread(h, 1, 8, f) != 8) { bad = 1; break; }
        n = le32(h + 4);
        if (n > end - off - 8) { bad = 1; break; }
        padded = n + (n & 1);
        if (padded > end - off - 8) { bad = 1; break; }
        if (!memcmp(h, "fmt ", 4)) {
            if (fmt || n < 16 || fread(h, 1, 16, f) != 16 ||
                le16(h) != 1 || le16(h + 2) != 1 || le32(h + 4) != 44100 ||
                le32(h + 8) != 88200 || le16(h + 12) != 2 || le16(h + 14) != 16) { bad = 1; break; }
            fmt = 1;
        } else if (!memcmp(h, "data", 4)) {
            if (data_off || !n || (n & 1) || n / 2 > MAX_FRAMES) { bad = 1; break; }
            data_off = off + 8;
            data_size = n;
        }
        off += 8 + padded;
    }
    if (bad || !fmt || !data_off || data_size > CACHE_BYTES - g_bytes) goto done;
    pcm = (short *)malloc(data_size);
    if (!pcm) goto done;
    if (fseek(f, data_off, SEEK_SET) || fread(pcm, 1, data_size, f) != data_size) {
        free(pcm); pcm = NULL; goto done;
    }
    /* PSP and the supported host harness are little endian. */
    *frames = data_size / 2;
done:
    fclose(f);
    return pcm;
}

static void clear_locked(void)
{
    int i;
    g_active = 0;
    g_voice = -1;
    g_pos = 0;
    for (i = 0; i < g_count; ++i) free(g_sounds[i].pcm);
    memset(g_sounds, 0, sizeof(g_sounds));
    g_count = 0;
    g_bytes = 0;
    g_plays = g_mixed = 0; g_peak = 0;
    g_error = "no sounds loaded";
}

void skin_sound_release(void)
{
    /* A timeout retains allocations rather than freeing a mixer-owned buffer.
     * The next release/set_root retries; no semaphore is destroyed mid-mix. */
    g_active = 0;
    if (!lock_sound(1)) return;
    clear_locked();
    unlock_sound();
}

struct Manifest {
    const char *root;
    char index[16], id[64], file[128], type[16];
    int bad, seen;
};
static int safe_file(const char *s)
{
    size_t n = strlen(s), i;
    if (n < 12 || strncmp(s, "assets/", 7) || strcmp(s + n - 4, ".wav")) return 0;
    for (i = 7; i < n - 4; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}
static void add_sound(struct Manifest *m)
{
    char path[512];
    struct Sound *s;
    int i, n;
    if (m->bad || m->seen != 7 || strcmp(m->type, "sound") || !m->id[0] ||
        strlen(m->id) > 32 || !safe_file(m->file) || g_count >= MAX_SOUNDS) return;
    for (i = 0; m->id[i]; ++i)
        if (!((m->id[i] >= 'a' && m->id[i] <= 'z') ||
              (m->id[i] >= 'A' && m->id[i] <= 'Z') ||
              (m->id[i] >= '0' && m->id[i] <= '9') ||
              m->id[i] == '_' || m->id[i] == '-')) return;
    for (i = 0; i < g_count; ++i) if (!strcmp(g_sounds[i].id, m->id)) return;
    n = snprintf(path, sizeof(path), "%s/%s", m->root, m->file);
    if (n < 0 || n >= (int)sizeof(path)) return;
    s = &g_sounds[g_count];
    s->pcm = load_wav(path, &s->frames);
    if (!s->pcm) { SOUND_LOG("skin sound load failed id=%s reason=%s", m->id, g_error); return; }
    strcpy(s->id, m->id);
    g_bytes += s->frames * 2;
    ++g_count;
    SOUND_LOG("skin sound loaded id=%s frames=%u", s->id, s->frames);
}
static void manifest_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    struct Manifest *m = (struct Manifest *)ud;
    const char *p, *key;
    char index[16], *dest;
    size_t n, cap;
    int bit;
    (void)num;
    if (strncmp(path, "assets.", 7)) return;
    p = path + 7;
    key = strchr(p, '.');
    if (!key || (n = (size_t)(key - p)) == 0 || n >= sizeof(index)) return;
    memcpy(index, p, n); index[n] = 0;
    for (size_t i = 0; i < n; ++i) if (index[i] < '0' || index[i] > '9') return;
    if (strcmp(index, m->index)) {
        add_sound(m);
        memset(m->id, 0, sizeof(m->id));
        memset(m->file, 0, sizeof(m->file));
        memset(m->type, 0, sizeof(m->type));
        m->seen = m->bad = 0;
        strcpy(m->index, index);
    }
    ++key;
    if (!strcmp(key, "id")) { dest = m->id; cap = sizeof(m->id); bit = 1; }
    else if (!strcmp(key, "file")) { dest = m->file; cap = sizeof(m->file); bit = 2; }
    else if (!strcmp(key, "type")) { dest = m->type; cap = sizeof(m->type); bit = 4; }
    else return;
    if ((m->seen & bit) || type != JSON_STR || !str || strlen(str) >= cap) m->bad = 1;
    else strcpy(dest, str);
    m->seen |= bit;
}

void skin_sound_set_root(const char *root)
{
    struct Manifest m;
    char path[512], *data;
    FILE *f;
    long n;
    int len;
#ifdef __PSP__
    /* Only main/UI calls set_root. Initialise before any voice is published. */
    if (g_lock < 0) g_lock = sceKernelCreateSema("skin_sound", 0, 1, 1, NULL);
#endif
    if (!lock_sound(1)) return;
    clear_locked();
    if (!root || !root[0]) { g_error = "no root"; goto done; }
    g_error = "manifest path too long";
    len = snprintf(path, sizeof(path), "%s/skin.json", root);
    if (len < 0 || len >= (int)sizeof(path)) goto done;
    f = fopen(path, "rb");
    if (!f) { g_error = "manifest open failed"; goto done; }
    g_error = "manifest read/size failed";
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || n > MANIFEST_BYTES || fseek(f, 0, SEEK_SET)) { fclose(f); goto done; }
    data = (char *)malloc((size_t)n + 1);
    if (!data) { fclose(f); goto done; }
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); goto done; }
    fclose(f);
    data[n] = 0;
    memset(&m, 0, sizeof(m)); m.root = root;
    if (json_validate(data, (int)n) == 0 && json_walk(data, (int)n, manifest_cb, &m) == 0) add_sound(&m);
    else { clear_locked(); g_error = "manifest JSON invalid"; }
    free(data);
done:
    if (g_count) g_error = "";
    SOUND_LOG("skin sound bank loaded=%d bytes=%u reason=%s", g_count, g_bytes, g_error);
    unlock_sound();
}

int skin_sound_play(const char *id)
{
    int i, ok = 0;
    if (!id) return 0;
    if (!lock_sound(0)) { g_error = "sound busy"; return 0; }
    for (i = 0; i < g_count; ++i) if (!strcmp(id, g_sounds[i].id)) {
        g_voice = i; g_pos = 0; g_active = 1; ++g_plays; g_error = ""; ok = 1; break;
    }
    if (!ok) g_error = g_count ? "sound id not loaded" : "no sounds loaded";
    unlock_sound();
    return ok;
}
void skin_sound_stop(void)
{
    g_active = 0;
    if (!lock_sound(1)) return;
    g_active = 0; g_voice = -1; g_pos = 0;
    unlock_sound();
}
int skin_sound_loaded_count(void) { return g_count; }
unsigned skin_sound_play_count(void) { return g_plays; }
unsigned skin_sound_mixed_frames(void) { return g_mixed; }
int skin_sound_peak(void) { return g_peak; }
const char *skin_sound_error(void) { return g_error; }
int skin_sound_active(void) { return g_active; }
void skin_sound_mix(short *pcm, int frames, int channels, int rate)
{
    int i, c;
    struct Sound *s;
    if (!pcm || frames <= 0 || (channels != 1 && channels != 2) || rate != 44100 || !lock_sound(0)) return;
    if (!g_active || g_voice < 0) { unlock_sound(); return; }
    s = &g_sounds[g_voice];
    for (i = 0; i < frames && g_pos < s->frames; ++i, ++g_pos) {
        int fx = s->pcm[g_pos] / 4;
        int amplitude = fx < 0 ? -fx : fx;
        if (fx) ++g_mixed;
        if (amplitude > g_peak) g_peak = amplitude;
        for (c = 0; c < channels; ++c) {
            int v = pcm[i * channels + c] + fx;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            pcm[i * channels + c] = (short)v;
        }
    }
    if (g_pos == s->frames) { g_active = 0; g_voice = -1; }
    unlock_sound();
}
