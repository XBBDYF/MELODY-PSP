"""拼音 / 日语假名输入法核心：宿主机回归。

直接编译 src/ime.c（它不依赖 PSP），检查：
- 拼音前缀查词、同音按词频排序
- 罗马字→平假名的特殊规则（ん / っ / ー / 小假名 / 浊音 / 拗音）
- 假名读音→汉字候选、假名原文与片假名兜底
- 退格、上屏、模式切换后的状态
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".tmp" / "ime-test"
OUT.mkdir(parents=True, exist_ok=True)

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ime.h"

static void type_str(const char *s)
{
    for (; *s; ++s) ime_feed((unsigned char)*s);
}

static int has_cand(const char *w)
{
    for (int i = 0; i < ime_candidate_count(); ++i)
        if (!strcmp(ime_candidate(i), w)) return 1;
    return 0;
}

int main(void)
{
    /* ---- 拼音 ---- */
    ime_set_mode(IME_PINYIN);
    type_str("zhoujielun");
    assert(!strcmp(ime_preedit(), "zhoujielun"));
    assert(ime_candidate_count() > 0);
    assert(!strcmp(ime_candidate(0), "周杰伦"));        /* freq 最高 */

    ime_reset();
    type_str("zhong");
    assert(has_cand("中国"));
    {
        char out[32];
        assert(ime_select(0, out, sizeof(out)) > 0);
        assert(!strcmp(out, "中国"));
        assert(!ime_active());                          /* 上屏后清空 */
    }

    ime_reset();
    type_str("zzzzzz");                                 /* 词库外不崩 */
    assert(ime_candidate_count() == 0);

    ime_reset();
    type_str("qingtian");
    assert(has_cand("晴天"));

    /* ---- 罗马字 → 假名 ---- */
    ime_set_mode(IME_KANA);
    type_str("sharuru");
    assert(!strcmp(ime_preedit(), "しゃるる"));

    ime_set_mode(IME_KANA);
    type_str("kitte");
    assert(!strcmp(ime_preedit(), "きって"));           /* 促音 */

    ime_set_mode(IME_KANA);
    type_str("nnhon");
    assert(!strcmp(ime_preedit(), "んほ"));             /* 末尾的 n 还在等下一个字符 */
    {
        char out[32];
        assert(ime_commit_raw(out, sizeof(out)) > 0);
        assert(!strcmp(out, "んほん"));                 /* 提交时才收成 ん */
    }

    ime_set_mode(IME_KANA);
    type_str("kanji");
    assert(!strcmp(ime_preedit(), "かんじ"));

    ime_set_mode(IME_KANA);
    type_str("ra-menn");
    assert(!strcmp(ime_preedit(), "らーめん"));         /* 长音 */

    ime_set_mode(IME_KANA);
    type_str("vanpai");
    assert(strstr(ime_preedit(), "ゔ") != NULL);        /* v → ゔ */

    /* ---- 假名 → 汉字 ---- */
    ime_set_mode(IME_KANA);
    type_str("yorunikakeru");
    assert(!strcmp(ime_preedit(), "よるにかける"));
    assert(has_cand("夜に駆ける"));

    ime_set_mode(IME_KANA);
    type_str("yakusoku");
    assert(has_cand("約束"));

    /* 词库没有的读音：至少保留假名本身与片假名 */
    ime_set_mode(IME_KANA);
    type_str("piyopiyo");
    assert(has_cand("ぴよぴよ"));
    assert(has_cand("ピヨピヨ"));

    /* 假名模式退格：先退罗马字，再退假名 */
    ime_set_mode(IME_KANA);
    type_str("ka");
    ime_backspace();
    assert(!ime_active());
    type_str("ka");
    ime_backspace();
    assert(!ime_active());

    /* 上屏后 preedit 清空，last_commit 可读 */
    ime_set_mode(IME_PINYIN);
    type_str("daoxiang");
    {
        char out[32];
        assert(ime_select(0, out, sizeof(out)) > 0);
        assert(!strcmp(ime_last_commit(), out));
        assert(!ime_active());
    }

    puts("PASS IME: pinyin prefix, romaji rules, kana dict, fallbacks, backspace");
    puts("LIMIT: 词库只覆盖常用词；长句组合/分词不在范围");
    return 0;
}
'''

c = OUT / "test.c"
c.write_text(HARNESS, encoding="utf-8")
exe = OUT / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src"), str(c), str(ROOT / "src/ime.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
