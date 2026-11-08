#include <string.h>
#include "search_history.h"

static char g_hist[SEARCH_HIST_MAX][SEARCH_HIST_LEN];
static int g_hist_n;

static void copy_q(char *dst, const char *src)
{
    int n = 0;

    while (src[n] && n < SEARCH_HIST_LEN - 1) {
        /* 去掉控制字符与竖线，避免破坏持久化格式 */
        unsigned char c = (unsigned char)src[n];

        dst[n] = (c == '|' || c == '\n' || c == '\r') ? ' ' : (char)c;
        n++;
    }
    dst[n] = 0;
}

void search_hist_init(const char *const *entries, int n)
{
    g_hist_n = 0;
    if (!entries) return;
    for (int i = 0; i < n && g_hist_n < SEARCH_HIST_MAX; ++i) {
        if (!entries[i] || !entries[i][0]) continue;
        copy_q(g_hist[g_hist_n], entries[i]);
        if (g_hist[g_hist_n][0]) g_hist_n++;
    }
}

int search_hist_push(const char *q)
{
    char buf[SEARCH_HIST_LEN];
    int found = -1;

    if (!q) return 0;
    copy_q(buf, q);
    if (!buf[0]) return 0;

    for (int i = 0; i < g_hist_n; ++i) {
        if (!strcmp(g_hist[i], buf)) {
            found = i;
            break;
        }
    }
    if (found == 0) return 0;               /* 已在最前，无变化 */

    if (found > 0) {                        /* 前移 */
        for (int i = found; i > 0; --i) memcpy(g_hist[i], g_hist[i - 1], SEARCH_HIST_LEN);
        memcpy(g_hist[0], buf, SEARCH_HIST_LEN);
        return 1;
    }
    if (g_hist_n < SEARCH_HIST_MAX) g_hist_n++;
    for (int i = g_hist_n - 1; i > 0; --i) memcpy(g_hist[i], g_hist[i - 1], SEARCH_HIST_LEN);
    memcpy(g_hist[0], buf, SEARCH_HIST_LEN);
    return 1;
}

int search_hist_count(void)
{
    return g_hist_n;
}

const char *search_hist_at(int i)
{
    if (i < 0 || i >= g_hist_n) return "";
    return g_hist[i];
}

void search_hist_clear(void)
{
    g_hist_n = 0;
}

int search_hist_export(char out[][SEARCH_HIST_LEN], int max)
{
    int n = 0;

    if (!out || max <= 0) return 0;
    for (int i = 0; i < g_hist_n && n < max; ++i) {
        memcpy(out[n], g_hist[i], SEARCH_HIST_LEN);
        n++;
    }
    return n;
}
