/*
 * MELODY PSP — 内置输入法（拼音 / 日语假名）
 *
 * 纯逻辑模块：不调用任何 PSP API，可被宿主机测试直接编译。
 *   IME_PINYIN：字母 → 全拼前缀 → 候选词（中文）
 *   IME_KANA  ：字母 → 罗马字转平假名 → 读音匹配 → 候选（汉字/片假名/假名本身）
 *
 * 约定：
 *   - 所有字符串都是 UTF-8；
 *   - 不分配内存，候选放在固定数组里；
 *   - 上屏由调用方负责写入搜索框（ime_select 返回文本）。
 */
#ifndef MELODY_IME_H
#define MELODY_IME_H

enum {
    IME_OFF = 0,
    IME_PINYIN = 1,
    IME_KANA = 2,
    IME_ASCII = 3        /* 英文直输：字母直接进搜索框，不产生 preedit */
};

#define IME_MAX_CAND   512   /* 候选一屏放不下，界面做横向滚动窗口 */
#define IME_PRE_MAX    48
#define IME_WORD_MAX   24

void ime_reset(void);

int  ime_mode(void);
void ime_set_mode(int mode);
void ime_toggle_mode(void);

int  ime_active(void);                 /* 有未上屏的输入 */
const char *ime_preedit(void);         /* 拼音：字母；假名：已转好的平假名 */

/* 输入一个字符：'a'..'z'、'-'（长音）、'\b'（退格）、' '（空格）。
 * 返回 1 表示这次输入把内容上屏了（调用方应读取 ime_last_commit()）。 */
int  ime_feed(int ch);
const char *ime_last_commit(void);

void ime_backspace(void);

int  ime_candidate_count(void);
const char *ime_candidate(int i);      /* i: 0..ime_candidate_count()-1 */

/* 选中第 i 个候选：写入 out，清空 preedit；返回写入字节数，0 表示失败 */
int  ime_select(int i, char *out, int cap);

/* 把当前 preedit 原样上屏（假名/英文场景的兜底） */
int  ime_commit_raw(char *out, int cap);

/* 本地词库联想：以 text（UTF-8）开头的词条，最多 max 条（用于「上屏后立即联想」） */
int  ime_assoc(const char *text, char out[][IME_WORD_MAX], int max);

#endif /* MELODY_IME_H */
