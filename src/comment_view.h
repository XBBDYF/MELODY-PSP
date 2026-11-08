#ifndef MELODY_COMMENT_VIEW_H
#define MELODY_COMMENT_VIEW_H
#include "app.h"
#include <stddef.h>
#define COMMENT_REPLY_MAX 32
#define COMMENT_LINE_HEIGHT 15
/* Measurement and rendering use the same iterator and font metrics. */
typedef int (*CommentMeasure)(const char *, int);
typedef void (*CommentGlyph)(int, int, const char *, int, void *);
typedef struct {
    const Comment *items; /* exactly one 32-slot worker-owned storage array */
    int count, total, more;
    long long cid, song;
} CommentReplies;
typedef struct {
    int selected;
    long long expanded;
    unsigned int generation;
    int page;
    int viewport; /* exact visible pixels supplied by screen_comments */
} CommentView;
CommentView *api_comment_view(void);
void api_request_comment_replies(long long song, long long cid);
void api_request_comment_count(long long cid);
const CommentReplies *api_comment_replies(long long cid);
int api_comment_replies_loading(long long cid);
int api_comment_replies_failed(long long cid);

static inline int comment_utf8_size(const char *s)
{
    unsigned char c = (unsigned char)*s;
    int n = c < 0x80 ? 1 : (c >= 0xc2 && c <= 0xdf ? 2 :
        (c >= 0xe0 && c <= 0xef ? 3 : (c >= 0xf0 && c <= 0xf4 ? 4 : 1)));
    int i;
    for (i = 1; i < n; ++i)
        if (!s[i] || ((unsigned char)s[i] & 0xc0) != 0x80) return 1;
    return n;
}
/* Returns line count, including explicit blank/trailing newline lines. */
static inline int comment_wrap(const char *text, int width, CommentMeasure measure,
                              CommentGlyph draw, void *ud)
{
    int x = 0, line = 0;
    const char *s = text ? text : "";
    if (width < 1) width = 1;
    while (*s) {
        int n = comment_utf8_size(s), w;
        if (*s == '\r') { ++s; continue; }
        if (*s == '\n') { x = 0; ++line; ++s; continue; }
        w = measure(s, n);
        if (w < 0) w = 0;
        if (x && x + w > width) { x = 0; ++line; }
        if (draw) draw(x, line * COMMENT_LINE_HEIGHT, s, n, ud);
        x += w; s += n;
    }
    return line + 1;
}
static inline int comment_body_height(const Comment *c, int width, CommentMeasure m)
{ return 36 + COMMENT_LINE_HEIGHT * comment_wrap(c->text, width, m, NULL, NULL); }
static inline int comment_row_height(const Comment *c, long long expanded,
                                    const CommentReplies *r, int width, CommentMeasure m)
{
    int h = comment_body_height(c, width, m), i;
    if (c->id && c->id == expanded) {
        h += 20; /* loading / empty / truncation summary */
        if (r) for (i = 0; i < r->count; ++i)
            h += comment_body_height(&r->items[i], width - 16, m);
    }
    return h;
}
static inline int comment_list_height(const CommentList *list, const CommentView *v,
                                     const CommentReplies *r, int width, CommentMeasure m)
{
    int h = 0, i;
    for (i = 0; i < list->count; ++i)
        h += comment_row_height(&list->items[i], v->expanded, r, width, m);
    return h;
}
static inline float comment_scroll_clamp(float scroll, int total, int viewport)
{
    int max = total > viewport ? total - viewport : 0;
    if (scroll < 0) return 0;
    if (scroll > max) return (float)max;
    return scroll;
}
static inline int comment_at_pixel(const CommentList *list, const CommentView *v,
                                  const CommentReplies *r, int width, CommentMeasure m, int pixel)
{
    int i, y = 0;
    for (i = 0; i < list->count; ++i) {
        y += comment_row_height(&list->items[i], v->expanded, r, width, m);
        if (pixel < y) return i;
    }
    return list->count ? list->count - 1 : 0;
}
#endif
