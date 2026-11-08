/*
 * MELODY PSP — 主题自带字体
 *
 * 主题可以在 skin.json 里声明自己的点阵字库（与内置 fontXX.bin 同格式 PSPF v1）：
 *
 *   "fonts": [
 *     { "size": 22, "file": "fonts/title.bin" },
 *     { "size": 16, "file": "fonts/ui.bin"    }
 *   ]
 *
 * 规则：
 *   - 只替换声明过的字号，其余继续用内置字库；
 *   - 主题字体缺字时自动回退到内置同字号（再走 32→22→16 的回退链）；
 *   - 载入失败整体放弃，保持内置，原因见 skin_font_last_error()；
 *   - 字体放在专用 arena（默认 1.5MB），切换主题时整体释放。
 */
#ifndef MELODY_SKIN_FONT_H
#define MELODY_SKIN_FONT_H

typedef struct {
    int size;              /* 12/16/22/32 */
    char file[64];         /* 相对主题根目录 */
} SkinFontDecl;

/* 从 skin.json 文本解析 fonts[]；返回条数，-1 表示格式错误。纯函数，可宿主机测试。 */
int skin_font_parse(const char *json, SkinFontDecl *out, int max);

/* 校验一个 PSPF 字库：头、边界、码点升序。返回 0 通过，<0 失败并写 err。纯函数。 */
int skin_font_validate(const unsigned char *blob, int len, char *err, int errlen);

/* 应用主题字体：读 skin.json → 逐个加载 → 挂到 gfx。成功返回载入的字号数。 */
int skin_font_apply(const char *root);

/* 释放所有主题字体（回到内置字库）。 */
void skin_font_release(void);

int skin_font_bytes(void);                 /* 当前占用字节数 */
const char *skin_font_last_error(void);    /* 最近一次失败原因（空 = 没有失败） */

#endif /* MELODY_SKIN_FONT_H */
