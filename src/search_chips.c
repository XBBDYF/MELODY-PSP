#include <string.h>
#include "search_chips.h"

static const char *const k_fallback[] = {
    "周杰伦", "林俊杰", "邓紫棋", "陈奕迅", "薛之谦", "李荣浩"
};

const char *search_chips_fallback(int i)
{
    int n = (int)(sizeof(k_fallback) / sizeof(k_fallback[0]));

    if (i < 0 || i >= n) return "";
    return k_fallback[i];
}

static int has(const char out[][SEARCH_CHIP_LEN], int n, const char *s)
{
    for (int i = 0; i < n; ++i)
        if (!strcmp(out[i], s)) return 1;
    return 0;
}

static int add(char out[][SEARCH_CHIP_LEN], int n, int max, const char *s)
{
    int len = 0;

    if (!s) return n;
    while (s[len] && len < SEARCH_CHIP_LEN - 1) len++;
    if (len == 0) return n;
    if (has(out, n, s)) return n;

    if (n < max) {
        memcpy(out[n], s, (size_t)len);
        out[n][len] = 0;
        return n + 1;
    }
    return n;
}

int search_chips_build(const char *const *recent_artists, int recent_n,
                       const char *const *fav_artists, int fav_n,
                       char out[][SEARCH_CHIP_LEN], int max)
{
    int n = 0;

    if (!out || max <= 0) return 0;
    for (int i = 0; i < recent_n && n < max; ++i) n = add(out, n, max, recent_artists[i]);
    for (int i = 0; i < fav_n && n < max; ++i) n = add(out, n, max, fav_artists[i]);
    for (int i = 0; n < max && i < SEARCH_CHIP_MAX; ++i) n = add(out, n, max, search_chips_fallback(i));
    return n;
}
