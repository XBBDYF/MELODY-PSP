"""主题字体：解析、校验、回退的宿主机回归。

- skin_font_parse：从 skin.json 文本取 fonts[]
- skin_font_validate：PSPF 头 / 边界 / 码点升序
- gfx 的主题字体层：缺字回退内置（用桩代替真实 gfx）
"""
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".tmp" / "skin-font-test"
OUT.mkdir(parents=True, exist_ok=True)

MAGIC = 0x46505350


def pspf(codepoints, w=8, h=8, rec=None):
    """构造一个合法的 PSPF v1 字库。"""
    stride = (w + 7) >> 3
    bits = bytes(stride * h)
    rec = rec or (12 + len(bits))
    body = bytearray()
    for cp in sorted(codepoints):
        r = bytearray(rec)
        struct.pack_into("<I", r, 0, cp)
        r[4] = w
        r[5] = h
        r[8:8 + len(bits)] = bits
        body += r
    return struct.pack("<5I", MAGIC, 1, len(codepoints), rec, 20) + bytes(body)


HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "skin_font.h"

static const char *json_ok =
    "{\"schema\":1,\"id\":\"metro\",\"fonts\":[{\"size\":22,\"file\":\"fonts/title.bin\"},"
    "{\"size\":16,\"file\":\"fonts/ui.bin\",\"name\":\"X\"}]}";

static const char *json_bad_size =
    "{\"fonts\":[{\"size\":21,\"file\":\"a.bin\"}]}";

static const char *json_no_fonts = "{\"schema\":1,\"id\":\"metro\"}";

/* 2 条记录 × 16 字节：cp(4) w(1) h(1) pad(2) bits(2) pad(6) */
static const unsigned char font_ok[] = {
    'P','S','P','F', 1,0,0,0, 2,0,0,0, 16,0,0,0, 20,0,0,0,
    0x41,0,0,0, 8,8, 0,0, 0xAA,0x55, 0,0,0,0,0,0,
    0x42,0,0,0, 8,8, 0,0, 0xCC,0x33, 0,0,0,0,0,0,
};

int main(void)
{
    SkinFontDecl d[4];
    char err[96];
    int n;

    /* 解析 */
    n = skin_font_parse(json_ok, d, 4);
    assert(n == 2);
    assert(d[0].size == 22 && !strcmp(d[0].file, "fonts/title.bin"));
    assert(d[1].size == 16 && !strcmp(d[1].file, "fonts/ui.bin"));
    assert(skin_font_parse(json_no_fonts, d, 4) == 0);
    assert(skin_font_parse(json_bad_size, d, 4) < 0);   /* 非法字号 */
    assert(skin_font_parse("{\"fonts\":[{\"size\":16}]}", d, 4) < 0); /* 缺 file */
    assert(skin_font_parse("{", d, 4) == 0);

    /* 校验 */
    assert(skin_font_validate(font_ok, (int)sizeof(font_ok), err, sizeof(err)) == 0);
    assert(skin_font_validate(font_ok, 8, err, sizeof(err)) < 0);        /* 太短 */
    {
        unsigned char bad[sizeof(font_ok)];
        memcpy(bad, font_ok, sizeof(bad));
        bad[0] = 'X';
        assert(skin_font_validate(bad, sizeof(bad), err, sizeof(err)) < 0);
    }
    {
        unsigned char bad[sizeof(font_ok)];
        memcpy(bad, font_ok, sizeof(bad));
        bad[12] = 8;                                                      /* rec 太小 */
        assert(skin_font_validate(bad, sizeof(bad), err, sizeof(err)) < 0);
    }
    {
        unsigned char bad[sizeof(font_ok)];
        memcpy(bad, font_ok, sizeof(bad));
        bad[20 + 16] = 0x40;                                              /* 第二条码点变小 */
        assert(skin_font_validate(bad, sizeof(bad), err, sizeof(err)) < 0);
        assert(strstr(err, "升序") != NULL);
    }
    puts("PASS skin font: parse decls, PSPF header, bounds, ascending codepoints");
    return 0;
}
'''

c = OUT / "test.c"
c.write_text(HARNESS, encoding="utf-8")
exe = OUT / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src"), "-DMELODY_HOST_TEST", str(c),
                str(ROOT / "src" / "skin_font.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)

# 生成一个真实字库（走 make_theme_font.py 的同一套格式）给后续 PPSSPP 用
sample = pspf(list(range(0x41, 0x4B)))
(OUT / "sample.bin").write_bytes(sample)
print("PASS sample font:", len(sample), "bytes")
