"""Generate the MELODY update splash images (480x272) and the gallery index.

Usage: python tools/make_update_splash.py [VERSION]   (default 4.1.1)

Reads releases/MELODY-<VERSION>/manifest.json for the revision (bundle CRC), which
the in-app gallery compares against, and writes the slides under
releases/MELODY-<VERSION>/COS更新预览/update-preview/<VERSION>/.
"""
from pathlib import Path
import json
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
VERSION = sys.argv[1] if len(sys.argv) > 1 else "4.1.1"
REL = ROOT / f"releases/MELODY-{VERSION}"
OUT = REL / f"COS更新预览/update-preview/{VERSION}"
OUT.mkdir(parents=True, exist_ok=True)

BG = (16, 16, 20)
TXT = (240, 241, 245)
MUTED = (150, 152, 162)
ACCENT = (197, 28, 48)
OK = (84, 212, 155)

W, H = 480, 272
FONT_T = "C:/Windows/Fonts/msyhbd.ttc"
FONT_B = "C:/Windows/Fonts/msyh.ttc"
f_title = ImageFont.truetype(FONT_T, 30)
f_sub = ImageFont.truetype(FONT_B, 14)
f_item = ImageFont.truetype(FONT_B, 14)
f_small = ImageFont.truetype(FONT_B, 11)

# 4.1.1 修复在最上面，4.1 的内容放在第二张。
SLIDES = {
    "4.1.1": [
        (f"MELODY {VERSION}", "本次修复", [
            "部分网络下未缓存歌曲无法播放",
            "DNS 解析失败时改用备用解析",
            "播放失败不再无限重试，两次即停",
            "主题切换失败会提示原因",
        ], "4.1.1 修复：DNS 备用解析、重试与提示"),
        (f"MELODY {VERSION}", "4.1 更新内容", [
            "MV 下载：网速不足时先下完再播",
            "下载进度与已下载字节一致",
            "歌曲提前下载更稳，切歌少等待",
            "网络连接：开机自动连上次配置",
            "第三方音源：无版权歌曲可走备用源",
        ], "4.1 更新：MV 下载、提前下载与网络"),
    ],
    "4.1": [
        (f"MELODY {VERSION}", "本次更新 · 下载与播放", [
            "MV 下载：网速不足时先下完再播",
            "下载进度与已下载字节一致",
            "歌曲提前下载更稳，切歌少等待",
            "连接页显示速率、所需带宽与剩余时间",
            "主题切换失败会重试并提示原因",
        ], "优化 MV 下载与歌曲提前下载"),
        (f"MELODY {VERSION}", "本次更新 · 网络与稳定", [
            "开机自动连接上次成功的网络",
            "首次联网不再白等十几秒",
            "播放失败不再反复重试",
            "新增设置：关闭本地 MV（VIDEO）搜索",
            "第三方音源：无版权歌曲可走备用音源",
        ], "网络连接与稳定性优化"),
    ],
}


def draw_slide(title, subtitle, items):
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, W, 3), fill=ACCENT)
    d.text((24, 18), title, font=f_title, fill=TXT)
    d.text((26, 56), subtitle, font=f_sub, fill=MUTED)
    d.line((24, 78, W - 24, 78), fill=(52, 54, 62), width=1)
    y = 92
    for line in items:
        d.text((26, y + 2), "·", font=f_item, fill=ACCENT)
        d.text((40, y), line, font=f_item, fill=TXT)
        y += 26
    d.text((26, H - 26), "更新内容以版本说明为准", font=f_small, fill=MUTED)
    d.text((W - 96, H - 26), "MELODY", font=f_small, fill=OK)
    return im


slides = SLIDES.get(VERSION, SLIDES["4.1"])
for i, (t, s, items, _caption) in enumerate(slides, start=1):
    draw_slide(t, s, items).save(OUT / f"{i:02d}.png")

manifest = {
    "schema": 1,
    "version": VERSION,
    "revision": json.loads((REL / "manifest.json").read_text(encoding="utf-8"))["bundle_crc32"],
    "slides": [{"file": f"{i:02d}.png", "caption": cap}
               for i, (_t, _s, _items, cap) in enumerate(slides, start=1)],
}
(OUT / "index.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
print("wrote", OUT)
print(json.dumps(manifest, ensure_ascii=False))
