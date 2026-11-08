/*
 * MELODY PSP — 超小 JSON 遍历器
 * ------------------------------------------------------------------
 * 不做 DOM，直接把「路径 -> 值」喂给回调，省内存也省代码：
 *
 *   {"playlist":{"tracks":[{"name":"屋顶","id":5257138}]}}
 *     -> cb("playlist.tracks.0.name", JSON_STR, "屋顶", 0)
 *     -> cb("playlist.tracks.0.id",   JSON_NUM, NULL, 5257138)
 *
 * 数组下标写成数字段。字符串会做反转义（\" \\ \/ \n \uXXXX）。
 */

#ifndef MELODY_JSON_H
#define MELODY_JSON_H

enum {
    JSON_STR = 1,
    JSON_NUM,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL
};

typedef void (*JsonCb)(void *ud, const char *path, int type, const char *str, long long num);

/* 返回 0 成功，<0 语法错误 */
int json_walk(const char *text, int len, JsonCb cb, void *ud);

/* Strict complete-document validation with no callbacks or side effects. */
int json_validate(const char *text, int len);

/* 把 "a.b.3.c" 拆成段，返回段数（最多 max 段） */
int json_split_path(const char *path, const char **seg, int *len, int max);

#endif
