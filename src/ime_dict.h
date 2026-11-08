/*
 * MELODY PSP — 内置输入法（拼音 / 日语假名）的字典类型
 *
 * 数据由 tools/build_ime_dict.py 从 tools/ime 目录下的 txt 生成到 ime_dict_data.h。
 * 运行时只读，不分配内存：
 *   - 拼音：key = 无声调全拼连写（zhoujielun），word = 中文词条
 *   - 假名：key = 平假名读音（よるにかける），word = 汉字/片假名写法
 * 两个数组都按 key 升序排列，查询用二分找区间再顺序扫。
 */
#ifndef MELODY_IME_DICT_H
#define MELODY_IME_DICT_H

typedef struct {
    const char *key;      /* 查询键（UTF-8） */
    const char *word;     /* 上屏文本（UTF-8） */
    unsigned char freq;   /* 1..9，越大越靠前 */
} ImeEntry;

#include "ime_dict_data.h"

#endif /* MELODY_IME_DICT_H */
