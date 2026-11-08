/*
 * MELODY PSP — 输入法核心（拼音 / 日语假名）
 *
 * 设计要点见 docs/IME.md：
 *   - 拼音键是「无声调全拼连写」，所以不需要切分音节，前缀匹配即可；
 *   - 日语先做罗马字→平假名（确定性），再用读音在词库里查汉字候选；
 *     不做分词/Viterbi，词条级匹配够用于搜索框；
 *   - 一切缓冲固定大小，按键路径上零分配。
 */
#include <stdio.h>
#include <string.h>
#include "ime.h"
#include "ime_dict.h"

#define CAND_MAX IME_MAX_CAND

static int g_mode = IME_PINYIN;
static char g_pre[IME_PRE_MAX];       /* 拼音：字母；假名：罗马字缓冲 */
static char g_kana[IME_PRE_MAX];      /* 假名：已转换的平假名（UTF-8） */
static char g_cand[CAND_MAX][IME_WORD_MAX];
static int  g_cand_n;
static char g_last[IME_PRE_MAX];

/* ------------------------------------------------------------------ */
/* 罗马字 → 平假名                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *romaji;
    const char *kana;
} RomajiRule;

static const RomajiRule k_romaji[] = {
    /* 拗音（长的放前面，按最长匹配） */
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
    {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"}, {"shi", "し"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"}, {"chi", "ち"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"ja", "じゃ"}, {"ju", "じゅ"}, {"jo", "じょ"}, {"ji", "じ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"dya", "ぢゃ"}, {"dyu", "ぢゅ"}, {"dyo", "ぢょ"},
    {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"},
    {"fa", "ふぁ"}, {"fi", "ふぃ"}, {"fu", "ふ"}, {"fe", "ふぇ"}, {"fo", "ふぉ"},
    {"va", "ゔぁ"}, {"vi", "ゔぃ"}, {"vu", "ゔ"}, {"ve", "ゔぇ"}, {"vo", "ゔぉ"},
    {"tsu", "つ"}, {"tu", "つ"},
    {"dzu", "づ"}, {"du", "づ"},
    /* 小假名 */
    {"la", "ぁ"}, {"li", "ぃ"}, {"lu", "ぅ"}, {"le", "ぇ"}, {"lo", "ぉ"},
    {"xa", "ぁ"}, {"xi", "ぃ"}, {"xu", "ぅ"}, {"xe", "ぇ"}, {"xo", "ぉ"},
    {"ltu", "っ"}, {"xtu", "っ"}, {"ltsu", "っ"}, {"xtsu", "っ"},
    {"lya", "ゃ"}, {"lyu", "ゅ"}, {"lyo", "ょ"},
    {"xya", "ゃ"}, {"xyu", "ゅ"}, {"xyo", "ょ"},
    {"lwa", "ゎ"}, {"xwa", "ゎ"},
    /* 五十音 */
    {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"sa", "さ"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"ta", "た"}, {"te", "て"}, {"to", "と"},
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha", "は"}, {"hi", "ひ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"},
    {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},
    {"wa", "わ"}, {"wo", "を"}, {"nn", "ん"}, {"n", "ん"},
    {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},
    {"za", "ざ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"da", "だ"}, {"de", "で"}, {"do", "ど"},
    {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},
    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"},
    {0, 0}
};

static int is_vowel(int ch)
{
    return ch == 'a' || ch == 'i' || ch == 'u' || ch == 'e' || ch == 'o';
}

/* 从 g_pre 开头尽量吃掉一段罗马字，转成假名追加到 g_kana。
 * 返回 1 表示这一轮有进展（需要再扫一次）。 */
static int kana_consume_one(void)
{
    int len = (int)strlen(g_pre);

    if (len <= 0) return 0;

    /* 长音符 */
    if (g_pre[0] == '-') {
        strncat(g_kana, "ー", sizeof(g_kana) - strlen(g_kana) - 1);
        memmove(g_pre, g_pre + 1, (size_t)len);
        return 1;
    }

    /* 促音：双写辅音（kk/tt/pp/ss...） */
    if (len >= 2 && g_pre[0] == g_pre[1] && !is_vowel(g_pre[0]) && g_pre[0] != 'n') {
        strncat(g_kana, "っ", sizeof(g_kana) - strlen(g_kana) - 1);
        memmove(g_pre, g_pre + 1, (size_t)len);
        return 1;
    }

    /* 拨音：n 后面跟辅音（不是元音、也不是 y）时先出 ん */
    if (g_pre[0] == 'n' && len >= 2 && !is_vowel(g_pre[1]) && g_pre[1] != 'y' && g_pre[1] != 'n') {
        strncat(g_kana, "ん", sizeof(g_kana) - strlen(g_kana) - 1);
        memmove(g_pre, g_pre + 1, (size_t)len);
        return 1;
    }

    /* 最长匹配规则表（表里 "n"/"nn" 放在最后，避免抢跑） */
    for (int i = 0; k_romaji[i].romaji; ++i) {
        int rl = (int)strlen(k_romaji[i].romaji);

        if (rl > len || strncmp(g_pre, k_romaji[i].romaji, (size_t)rl)) continue;
        /* 单字母 "n" 不即时转换：等下一个字符决定 な行 还是 ん */
        if (!strcmp(k_romaji[i].romaji, "n")) continue;
        strncat(g_kana, k_romaji[i].kana, sizeof(g_kana) - strlen(g_kana) - 1);
        memmove(g_pre, g_pre + rl, (size_t)(len - rl + 1));
        return 1;
    }
    return 0;
}

static void kana_flush_romaji(void)
{
    while (kana_consume_one()) {
    }
    /* 剩下的无法转换：原样留在 g_pre 里，作为英文/半角候选 */
}

/* 平假名 → 片假名（U+3041..U+3096 → +0x60；ゔ 同样适用） */
static void hira_to_kata(const char *src, char *out, int cap)
{
    const unsigned char *p = (const unsigned char *)src;
    int n = 0;

    while (*p && n < cap - 4) {
        unsigned int c;

        if (*p < 0x80) {
            out[n++] = (char)*p++;
            continue;
        }
        if ((*p & 0xE0) == 0xC0 && p[1]) {
            c = ((unsigned)(*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            c = ((unsigned)(*p & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            out[n++] = (char)*p++;
            continue;
        }
        if (c >= 0x3041 && c <= 0x3096 && c != 0x3094) c += 0x60;
        else if (c == 0x3094) c = 0x30F4;

        if (c < 0x800) {
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
}

/* ------------------------------------------------------------------ */
/* 候选                                                                */
/* ------------------------------------------------------------------ */

static void cand_clear(void)
{
    g_cand_n = 0;
}

static int cand_has(const char *w)
{
    for (int i = 0; i < g_cand_n; ++i)
        if (!strcmp(g_cand[i], w)) return 1;
    return 0;
}

static void cand_add(const char *w)
{
    if (!w || !w[0]) return;
    if (strlen(w) >= IME_WORD_MAX) return;
    if (g_cand_n >= CAND_MAX) return;
    if (cand_has(w)) return;
    snprintf(g_cand[g_cand_n++], IME_WORD_MAX, "%s", w);
}

/* 从有序表里取 [lo,hi)：key 前缀区间 */
static void range(const ImeEntry *tab, int n, const char *key, int *lo, int *hi)
{
    int a = 0, b = n, kl = (int)strlen(key);

    while (a < b) {
        int mid = (a + b) / 2;

        if (strncmp(tab[mid].key, key, (size_t)kl) < 0) a = mid + 1;
        else b = mid;
    }
    *lo = a;
    b = n;
    while (a < b) {
        int mid = (a + b) / 2;

        if (strncmp(tab[mid].key, key, (size_t)kl) <= 0) a = mid + 1;
        else b = mid;
    }
    *hi = a;
}

/* 收集前缀（或完全匹配）候选，按频率降序插入，再按顺序进候选表 */
static void collect(const ImeEntry *tab, int n, const char *key, int exact_only)
{
    int lo, hi, i, k, kl = (int)strlen(key);
    const ImeEntry *picked[CAND_MAX];
    int picked_n = 0;

    range(tab, n, key, &lo, &hi);
    if (hi - lo > 64) hi = lo + 64;
    for (i = lo; i < hi; ++i) {
        int pos;

        if (exact_only) {
            if (strcmp(tab[i].key, key)) continue;
        } else if (strncmp(tab[i].key, key, (size_t)kl)) {
            continue;
        }

        pos = picked_n < CAND_MAX ? picked_n : CAND_MAX;
        while (pos > 0 && (picked[pos - 1]->freq < tab[i].freq ||
                           (picked[pos - 1]->freq == tab[i].freq &&
                            strcmp(picked[pos - 1]->key, tab[i].key) > 0))) {
            if (pos < CAND_MAX) picked[pos] = picked[pos - 1];
            pos--;
        }
        if (pos < CAND_MAX) {
            picked[pos] = &tab[i];
            if (picked_n < CAND_MAX) picked_n++;
        }
    }
    for (k = 0; k < picked_n; ++k) cand_add(picked[k]->word);
}

/* 结尾孤立的 n 在提交时才转成 ん（避免 "na" 被拆成 ん+あ） */
/* 单字候选：key 是音节。exact=1 只要同音节，exact=0 收以 prefix 开头的音节。 */
static void char_candidates(const char *prefix, int exact, int max)
{
    int lo, hi, i, added = 0, kl = (int)strlen(prefix);

    if (!prefix[0] || max <= 0) return;
    {
        int a = 0, b = k_ime_chars_count;

        while (a < b) {
            int mid = (a + b) / 2;

            if (strncmp(k_ime_chars[mid].key, prefix, (size_t)kl) < 0) a = mid + 1;
            else b = mid;
        }
        lo = a;
        b = k_ime_chars_count;
        while (a < b) {
            int mid = (a + b) / 2;

            if (strncmp(k_ime_chars[mid].key, prefix, (size_t)kl) <= 0) a = mid + 1;
            else b = mid;
        }
        hi = a;
    }

    for (i = lo; i < hi && added < max; ++i) {
        const char *c = k_ime_chars[i].chars;

        if (exact && strcmp(k_ime_chars[i].key, prefix)) continue;
        while (*c && added < max) {
            char one[8];
            int n = 1;

            if ((*c & 0xE0) == 0xC0) n = 2;
            else if ((*c & 0xF0) == 0xE0) n = 3;
            else if ((*c & 0xF8) == 0xF0) n = 4;
            memcpy(one, c, (size_t)n);
            one[n] = 0;
            cand_add(one);
            c += n;
            added++;
        }
    }
}

/* 词库里的首字：输入单字母时给几个「相关单字」而不是只有整词 */
static void word_initial_candidates(const char *prefix, int max)
{
    int lo, hi, i, added = 0, kl = (int)strlen(prefix);

    if (!prefix[0] || max <= 0) return;
    range(k_ime_pinyin, k_ime_pinyin_count, prefix, &lo, &hi);
    for (i = lo; i < hi && added < max; ++i) {
        const char *w = k_ime_pinyin[i].word;
        char one[8];
        int n = 1;

        if (!w[0]) continue;
        if ((*w & 0xE0) == 0xC0) n = 2;
        else if ((*w & 0xF0) == 0xE0) n = 3;
        else if ((*w & 0xF8) == 0xF0) n = 4;
        if (n == 1) continue;                 /* 跳过 ASCII */
        memcpy(one, w, (size_t)n);
        one[n] = 0;
        cand_add(one);
        added++;
        (void)kl;
    }
}

static void rebuild_candidates(void);

static void kana_finish(void)
{
    if (g_mode != IME_KANA) return;
    if (!strcmp(g_pre, "n")) {
        strncat(g_kana, "ん", sizeof(g_kana) - strlen(g_kana) - 1);
        g_pre[0] = 0;
        rebuild_candidates();
    }
}

static void rebuild_candidates(void)
{
    cand_clear();

    if (g_mode == IME_PINYIN) {
        if (!g_pre[0]) return;
        /* 顺序：整词完全匹配 → 同音节单字 → 词首字 → 前缀词 */
        collect(k_ime_pinyin, k_ime_pinyin_count, g_pre, 1);
        if (g_cand_n < 5) char_candidates(g_pre, 1, 5 - g_cand_n);
        if (g_cand_n < 5) word_initial_candidates(g_pre, 5 - g_cand_n);
        collect(k_ime_pinyin, k_ime_pinyin_count, g_pre, 0);
        return;
    }

    /* 假名模式 */
    if (g_kana[0]) {
        char kata[IME_PRE_MAX];

        collect(k_ime_kana, k_ime_kana_count, g_kana, 1);   /* 完全匹配 */
        if (g_cand_n < CAND_MAX) collect(k_ime_kana, k_ime_kana_count, g_kana, 0); /* 前缀 */
        cand_add(g_kana);                                 /* 假名本身 */
        hira_to_kata(g_kana, kata, (int)sizeof(kata));
        cand_add(kata);                                   /* 片假名 */
    }
    if (g_pre[0]) cand_add(g_pre);                        /* 未转换的罗马字/英文 */
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void ime_reset(void)
{
    g_pre[0] = 0;
    g_kana[0] = 0;
    cand_clear();
}

int ime_mode(void)
{
    return g_mode;
}

void ime_set_mode(int mode)
{
    if (mode != IME_OFF && mode != IME_PINYIN && mode != IME_KANA && mode != IME_ASCII) return;
    ime_reset();
    g_mode = mode;
}

void ime_toggle_mode(void)
{
    /* 循环：拼音 → 假名 → 英文 → 拼音 */
    if (g_mode == IME_PINYIN) ime_set_mode(IME_KANA);
    else if (g_mode == IME_KANA) ime_set_mode(IME_ASCII);
    else ime_set_mode(IME_PINYIN);
}

int ime_active(void)
{
    return g_pre[0] || g_kana[0];
}

const char *ime_preedit(void)
{
    return g_mode == IME_KANA ? g_kana : g_pre;
}

const char *ime_last_commit(void)
{
    return g_last;
}

int ime_candidate_count(void)
{
    return g_cand_n;
}

const char *ime_candidate(int i)
{
    if (i < 0 || i >= g_cand_n) return "";
    return g_cand[i];
}

int ime_feed(int ch)
{
    int len = (int)strlen(g_pre);

    g_last[0] = 0;

    /* 英文直输：不消费按键，调用方把字母写进搜索框 */
    if (g_mode == IME_ASCII) return 2;

    if (ch == '\b' || ch == 127) {
        ime_backspace();
        return 0;
    }
    if (ch == ' ' || ch == '\n' || ch == '\r') {
        char out[IME_WORD_MAX];

        kana_finish();
        int n = g_cand_n > 0 ? ime_select(0, out, (int)sizeof(out))
                             : ime_commit_raw(out, (int)sizeof(out));
        (void)n;
        return 1;
    }
    if (ch == '-') {
        if (len + 1 < IME_PRE_MAX) {
            g_pre[len] = '-';
            g_pre[len + 1] = 0;
        }
        if (g_mode == IME_KANA) kana_flush_romaji();
        rebuild_candidates();
        return 0;
    }
    if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
    if (ch < 'a' || ch > 'z') return 0;

    if (len + 1 >= IME_PRE_MAX) return 0;
    g_pre[len] = (char)ch;
    g_pre[len + 1] = 0;

    if (g_mode == IME_KANA) kana_flush_romaji();
    rebuild_candidates();
    return 0;
}

void ime_backspace(void)
{
    int len = (int)strlen(g_pre);

    g_last[0] = 0;
    if (g_mode == IME_KANA) {
        /* 先退罗马字，再退假名（UTF-8 安全退一个字符） */
        if (len > 0) {
            g_pre[len - 1] = 0;
        } else {
            int kl = (int)strlen(g_kana);

            if (kl > 0) {
                kl--;
                while (kl > 0 && ((unsigned char)g_kana[kl] & 0xC0) == 0x80) kl--;
                g_kana[kl] = 0;
            }
        }
    } else if (len > 0) {
        g_pre[len - 1] = 0;
    }
    rebuild_candidates();
}

int ime_commit_raw(char *out, int cap)
{
    const char *src;

    kana_finish();
    src = g_mode == IME_KANA ? g_kana : g_pre;

    if (!src[0] || cap <= 1) return 0;
    snprintf(out, (size_t)cap, "%s", src);
    snprintf(g_last, sizeof(g_last), "%s", src);
    ime_reset();
    return (int)strlen(out);
}

int ime_select(int i, char *out, int cap)
{
    if (i < 0 || i >= g_cand_n || cap <= 1) return 0;
    snprintf(out, (size_t)cap, "%s", g_cand[i]);
    snprintf(g_last, sizeof(g_last), "%s", g_cand[i]);
    ime_reset();
    return (int)strlen(out);
}
