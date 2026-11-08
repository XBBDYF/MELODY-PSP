"""Generate the three support buttons in docs/assets/.

- badge-afdian.png   爱发电（使用平台官方白色 logo + 品牌紫渐变）
- badge-site.png     作者官网
- badge-bilibili.png B 站

需要 Pillow 与中文字体（Windows 自带 msyh.ttc）。Afdian 的 logo 不随仓库分发，
脚本运行时从 static.afdiancdn.com 下载；离网时会退回一个占位圆点。

用法：python -B tools/make_support_badges.py
"""
from pathlib import Path
import sys
import urllib.request

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "assets"
OUT.mkdir(parents=True, exist_ok=True)

AFDIAN_LOGO_URL = "https://static.afdiancdn.com/static/img/logo/logo_white.png"
AFDIAN_LOGO = ROOT / ".tmp" / "afdian-logo-white.png"

FONT_B = "C:/Windows/Fonts/msyhbd.ttc"
FONT_R = "C:/Windows/Fonts/msyh.ttc"
S = 2
H = 56 * S
PAD = 16 * S
ICON = 30 * S
GAP = 12 * S
RADIUS = 10 * S
SEP = 1 * S

f_brand = ImageFont.truetype(FONT_B, 21 * S)
f_value = ImageFont.truetype(FONT_R, 15 * S)


def tw(font, s):
    return int(font.getlength(s))


def glyph(d, kind, cx, cy, r):
    white = (255, 255, 255, 255)
    if kind == "globe":
        w = max(2, S * 2)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=white, width=w)
        d.ellipse((cx - r * 0.42, cy - r, cx + r * 0.42, cy + r), outline=white, width=max(2, S))
        d.line((cx - r, cy, cx + r, cy), fill=white, width=max(2, S))
        d.line((cx, cy - r, cx, cy + r), fill=white, width=max(2, S))
    elif kind == "play":
        rr = r * 1.15
        d.rounded_rectangle((cx - rr, cy - rr * 0.74, cx + rr, cy + rr * 0.74),
                            radius=rr * 0.3, fill=white)
        d.polygon([(cx - rr * 0.28, cy - rr * 0.4), (cx - rr * 0.28, cy + rr * 0.4),
                   (cx + rr * 0.46, cy)], fill=(251, 114, 153))


def download_logo():
    if AFDIAN_LOGO.exists():
        return AFDIAN_LOGO
    AFDIAN_LOGO.parent.mkdir(parents=True, exist_ok=True)
    try:
        urllib.request.urlretrieve(AFDIAN_LOGO_URL, AFDIAN_LOGO)
        return AFDIAN_LOGO
    except Exception as exc:  # offline or blocked
        print("warn: cannot fetch Afdian logo:", exc, file=sys.stderr)
        return None


def button(path, brand, value, top, bottom, glyph_kind=None, logo=None):
    width = PAD + ICON + GAP + tw(f_brand, brand) + 10 * S + SEP + 10 * S + tw(f_value, value) + PAD
    im = Image.new("RGBA", (width, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    for y in range(H):
        t = y / (H - 1)
        d.line((0, y, width, y),
               fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))

    mask = Image.new("L", (width, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, width - 1, H - 1), RADIUS, fill=255)
    body = Image.new("RGBA", (width, H), (0, 0, 0, 0))
    body.paste(im, (0, 0), mask)
    im = body
    d = ImageDraw.Draw(im)

    d.rounded_rectangle((0, 0, width - 1, H - 1), RADIUS, outline=(255, 255, 255, 46), width=S)
    d.line((RADIUS, 1 * S, width - RADIUS, 1 * S), fill=(255, 255, 255, 40), width=S)

    ix, iy = PAD, (H - ICON) // 2
    if logo:
        mark = Image.open(logo).convert("RGBA")
        scale = min(ICON / mark.width, (ICON * 0.92) / mark.height)
        mark = mark.resize((max(1, int(mark.width * scale)), max(1, int(mark.height * scale))),
                           Image.LANCZOS)
        im.alpha_composite(mark, (ix + (ICON - mark.width) // 2, iy + (ICON - mark.height) // 2))
    elif glyph_kind:
        glyph(d, glyph_kind, ix + ICON // 2, H // 2, ICON * 0.34)

    x = PAD + ICON + GAP
    d.text((x, H // 2 - 26 * S), brand, font=f_brand, fill=(255, 255, 255, 255))
    sep_x = x + tw(f_brand, brand) + 10 * S
    d.line((sep_x, H // 2 - 12 * S, sep_x, H // 2 + 12 * S), fill=(255, 255, 255, 70), width=SEP)
    d.text((sep_x + 10 * S, H // 2 - 9 * S), value, font=f_value, fill=(255, 255, 255, 225))

    im.save(path)
    return im.size


def main():
    logo = download_logo()
    print(button(OUT / "badge-afdian.png", "爱发电", "支持开发",
                 (166, 116, 250), (124, 77, 245), logo=logo))
    print(button(OUT / "badge-site.png", "作者官网", "xbbdyf.github.io/retro",
                 (56, 60, 74), (32, 35, 44), glyph_kind="globe"))
    print(button(OUT / "badge-bilibili.png", "B 站", "535975174",
                 (255, 138, 171), (251, 114, 153), glyph_kind="play"))


if __name__ == "__main__":
    main()
