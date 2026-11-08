/*
 * MELODY PSP — 搜索历史（纯逻辑，宿主机可测）
 *
 * 最近搜索最多 SEARCH_HIST_MAX 条，新的在前；重复词只前移不重复。
 * 持久化由 store.c 负责（data.txt 里的 H|query 行）。
 */
#ifndef MELODY_SEARCH_HISTORY_H
#define MELODY_SEARCH_HISTORY_H

#define SEARCH_HIST_MAX 8
#define SEARCH_HIST_LEN 24

/* 载入时重置；entries 可以为空数组 */
void search_hist_init(const char *const *entries, int n);

/* 返回 1 = 内容有变化（调用方负责落盘），0 = 空串/无变化 */
int  search_hist_push(const char *q);

int  search_hist_count(void);
const char *search_hist_at(int i);

void search_hist_clear(void);

/* 导出到 out[][SEARCH_HIST_LEN]，返回条数 */
int  search_hist_export(char out[][SEARCH_HIST_LEN], int max);

#endif /* MELODY_SEARCH_HISTORY_H */
