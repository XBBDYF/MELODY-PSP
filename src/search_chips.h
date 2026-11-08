/*
 * MELODY PSP — 搜索页的「推荐搜索」（纯逻辑，宿主机可测）
 *
 * 以前的推荐是写死的五个歌手；现在按优先级拼：
 *   1. 最近播放的歌手（去重，最多 recent_n 个）
 *   2. 收藏歌曲的歌手
 *   3. 内置兜底（保证页面上永远有东西可点）
 * 结果写进 out[][SEARCH_CHIP_LEN]，最多 max 个。
 */
#ifndef MELODY_SEARCH_CHIPS_H
#define MELODY_SEARCH_CHIPS_H

#define SEARCH_CHIP_MAX 6
#define SEARCH_CHIP_LEN 24

int search_chips_build(const char *const *recent_artists, int recent_n,
                       const char *const *fav_artists, int fav_n,
                       char out[][SEARCH_CHIP_LEN], int max);

/* 只在没有历史/推荐时用 */
const char *search_chips_fallback(int i);

#endif /* MELODY_SEARCH_CHIPS_H */
