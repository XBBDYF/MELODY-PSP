#ifndef MELODY_PLAYBACK_CONTEXT_H
#define MELODY_PLAYBACK_CONTEXT_H

#include "app.h"
#include "cache.h"
#include "next_plan.h"
#include <stdio.h>
#include <string.h>

#define PLAYBACK_LOCAL_MAX 256
#define PLAYBACK_PATH_MAX 256

typedef enum {
    PLAYBACK_NONE, PLAYBACK_ONLINE, PLAYBACK_CACHE, PLAYBACK_LOCAL
} PlaybackSource;

/* One active source, independent of the screen's mutable browsing list.
 * The union keeps the local path snapshot inside the online list's budget. */
typedef struct {
    PlaybackSource source;
    int index, count;
    unsigned random;
    union {
        SongList online;
        long long cache_ids[CACHE_MAX];
        char paths[PLAYBACK_LOCAL_MAX][PLAYBACK_PATH_MAX];
    } data;
} PlaybackContext;

static inline void playback_context_online(PlaybackContext *p, const SongList *list, int index)
{
    p->source = PLAYBACK_ONLINE;
    p->data.online = *list;
    p->count = list->count;
    p->index = index;
}

static inline void playback_context_cache(PlaybackContext *p, const CacheEntry *items, int count, int index)
{
    p->source = PLAYBACK_CACHE;
    p->count = count < CACHE_MAX ? count : CACHE_MAX;
    p->index = index;
    for (int i = 0; i < p->count; ++i) p->data.cache_ids[i] = items[i].id;
}

static inline void playback_context_local(PlaybackContext *p, int count, int index,
                                          const char *(*path_of)(int))
{
    p->source = PLAYBACK_LOCAL;
    p->count = count < PLAYBACK_LOCAL_MAX ? count : PLAYBACK_LOCAL_MAX;
    p->index = index;
    for (int i = 0; i < p->count; ++i) {
        const char *path = path_of(i);
        snprintf(p->data.paths[i], PLAYBACK_PATH_MAX, "%s", path ? path : "");
    }
}

/* Resolve stable cache IDs against a fresh snapshot, never its former row. */
static inline int playback_context_cache_row(const PlaybackContext *p, int index,
                                             const CacheEntry *items, int count)
{
    if (p->source != PLAYBACK_CACHE || index < 0 || index >= p->count) return -1;
    for (int i = 0; i < count; ++i)
        if (items[i].id == p->data.cache_ids[index]) return i;
    return -1;
}

/* Manual controls always advance, including loop-one. EOF loop-one remains
 * audio_seek(0); sequential EOF and manual next both stop at the last item. */
static inline int playback_context_next(PlaybackContext *p, NextPlan *plan, int dir, int mode)
{
    if (p->source == PLAYBACK_NONE || p->count <= 0 || p->index < 0 || p->index >= p->count) return -1;
    if (dir < 0) return p->index > 0 ? p->index - 1 : p->count - 1;
    if (p->source == PLAYBACK_ONLINE && mode != 2)
        return next_plan_index(plan, &p->data.online, p->index, mode);
    if (mode == 1 && p->count > 1) {
        if (!p->random) p->random = 2463534242u;
        p->random ^= p->random << 13;
        p->random ^= p->random >> 17;
        p->random ^= p->random << 5;
        return (p->index + 1 + p->random % (unsigned)(p->count - 1)) % p->count;
    }
    return p->index + 1 < p->count ? p->index + 1 : -1;
}
#endif
