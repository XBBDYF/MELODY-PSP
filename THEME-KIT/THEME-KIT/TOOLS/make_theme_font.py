"""Theme font support: build a PSPF v1 bitmap font from a TTF/OTF.

Format (same as the built-in src/fontXX.bin, so the runtime needs no new code):

    u32 magic 'PSPF' | u32 version=1 | u32 count | u32 rec_size | u32 records_offset
    records (sorted by codepoint): u32 codepoint | u8 w | u8 h | 1bpp bitmap (w*h bits, padded per row)

Usage:
    python -B tools/make_theme_font.py font.ttf --size 22 --charset gbk -o out.bin
    python -B tools/make_theme_font.py font.ttf --size 16 --chars "MELODY开始" -o out.bin
    python -B tools/make_theme_font.py font.ttf --size 22 --charset gbk --test-text "夜に駆ける"
"""
from pathlib import Path
import argparse
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

MAGIC = 0x46505350  # "PSPF" little-endian

# 字集可选范围
CHARSETS = {
    "ascii": lambda: [chr(c) for c in range(0x20, 0x7F)],
    "kana": lambda: [chr(c) for c in range(0x3041, 0x30FF)],
    "gbk": None,       # 用内置 font16 的码点表（等于 GBK 全字集）
    "gb2312": None,    # 用内置 font22 的码点表
}


def load_builtin_codepoints(path):
    b = path.read_bytes()
    magic, ver, count, rec, off = struct.unpack_from("<5I", b, 0)
    assert magic == MAGIC
    return [struct.unpack_from("<I", b, off + i * rec)[0] for i in range(count)]


def charset_codepoints(name, root):
    if name in ("gbk", "gb2312"):
        f = root / "src" / ("font16.bin" if name == "gbk" else "font22.bin")
        return load_builtin_codepoints(f)
    fn = CHARSETS.get(name)
    if fn is None:
        sys.exit(f"unknown charset {name}")
    return [ord(c) for c in fn()]


def render(font, cp, size):
    """渲染一个码点为 1bpp 位图，返回 (w, h, bytes, advance)。"""
    ch = chr(cp)
    bbox = font.getbbox(ch)
    adv = int(round(font.getlength(ch)))
    if bbox is None:
        return 0, 0, b"", adv
    x0, y0, x1, y1 = bbox
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return 0, 0, b"", adv
    im = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(im)
    d.text((-x0, -y0), ch, font=font, fill=255)
    # 1bpp 阈值；行按字节对齐
    stride = (w + 7) >> 3
    bits = bytearray(stride * h)
    px = im.load()
    for y in range(h):
        for x in range(w):
            if px[x, y] >= 110:
                bits[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
    # 记录里只有 w/h + 位图；行间距由 line_h 决定（调用方按字号设置）
    return w, h, bytes(bits), adv


def build(ttf, size, codepoints, out, threshold_missing=0):
    font = ImageFont.truetype(str(ttf), size)
    recs = []
    missing = 0
    for cp in sorted(set(codepoints)):
        if cp < 0x20:
            continue
        w, h, bits, _adv = render(font, cp, size)
        if w <= 0 or h <= 0:
            missing += 1
            continue
        # rec = u32 cp + u8 w + u8 h + 6 bytes padding + 位图
        rec_size = 12 + len(bits)
        recs.append((cp, w, h, bits, rec_size))

    if not recs:
        sys.exit("no glyph rendered")

    # 统一 rec_size（取最大值，短记录补零），运行时按固定 stride 二分
    rec_size = max(r[4] for r in recs)
    count = len(recs)
    offset = 20
    body = bytearray()
    for cp, w, h, bits, _rs in recs:
        rec = bytearray(rec_size)
        struct.pack_into("<I", rec, 0, cp)
        rec[4] = w
        rec[5] = h
        rec[8:8 + len(bits)] = bits
        body += rec

    header = struct.pack("<5I", MAGIC, 1, count, rec_size, offset)
    data = header + bytes(body)
    Path(out).write_bytes(data)

    print(f"font: {ttf} size={size}px")
    print(f"glyphs: {count} (skipped {missing}), rec={rec_size}B, total={len(data)} bytes")
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ttf")
    ap.add_argument("--size", type=int, required=True)
    ap.add_argument("--charset", default="gbk")
    ap.add_argument("--chars", default="")
    ap.add_argument("--chars-file", default="")
    ap.add_argument("--text-file", default="", help="只包含该文本里出现的字")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[1]
    if args.text_file:
        text = Path(args.text_file).read_text(encoding="utf-8")
        cps = [ord(c) for c in sorted(set(text))]
    elif args.chars_file:
        text = Path(args.chars_file).read_text(encoding="utf-8")
        cps = [ord(c) for c in sorted(set(text))]
    elif args.chars:
        cps = [ord(c) for c in sorted(set(args.chars))]
    else:
        cps = charset_codepoints(args.charset, root)
        if args.size >= 22 and len(cps) > 9000:
            print(f"warn: {args.size}px 用 GBK 全字集约 {len(cps) * args.size * args.size // 8 // 1024}KB，"
                  f"主题字体建议只带需要的字", file=sys.stderr)

    build(args.ttf, args.size, cps, args.out)


if __name__ == "__main__":
    main()
