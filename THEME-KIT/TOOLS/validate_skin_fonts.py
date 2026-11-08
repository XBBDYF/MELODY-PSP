"""Theme font validator: skin.json fonts[] + PSPF file checks.

Usage: python -B tools/validate_skin_fonts.py skins/metro [more dirs]

Mirrors the runtime rules in src/skin_font.c so a theme author can catch problems
before installing the theme on a PSP:
  - size must be 12/16/22/32, file must exist under the skin directory
  - PSPF v1 header, record size, offset/count bounds, codepoints strictly ascending
  - total declared font bytes <= 1.5MB
  - when a "license" field is present, fonts/LICENSE.txt must exist
"""
from pathlib import Path
import json
import struct
import sys

MAGIC = 0x46505350
VALID_SIZES = {12, 16, 22, 32}
BUDGET = 1536 * 1024


def check_font(path, rel):
    data = path.read_bytes()
    if len(data) < 20:
        return [f"{rel}: 文件太短（{len(data)}B）"]
    magic, version, count, rec, off = struct.unpack_from("<5I", data, 0)
    errs = []
    if magic != MAGIC:
        errs.append(f"{rel}: 不是 PSPF 字库")
        return errs
    if version != 1:
        errs.append(f"{rel}: 版本 {version} 不支持")
    if not (12 <= rec <= 4096):
        errs.append(f"{rel}: 记录长度 {rec} 非法")
        return errs
    if not (0 < count <= 22000):
        errs.append(f"{rel}: 字数 {count} 非法")
    if off > len(data) or count > (len(data) - off) // rec:
        errs.append(f"{rel}: 记录越界（off={off} count={count} rec={rec}）")
        return errs
    prev = None
    for i in range(count):
        (cp,) = struct.unpack_from("<I", data, off + i * rec)
        if prev is not None and cp <= prev:
            errs.append(f"{rel}: 码点未升序（第 {i} 条）")
            break
        prev = cp
    return errs


def check_skin(root):
    root = Path(root)
    errs = []
    try:
        manifest = json.loads((root / "skin.json").read_text(encoding="utf-8"))
    except Exception as exc:
        return [f"{root.name}: skin.json 读取失败（{exc}）"]

    fonts = manifest.get("fonts") or []
    if not isinstance(fonts, list):
        return [f"{root.name}: fonts 必须是数组"]
    if len(fonts) > 4:
        errs.append(f"{root.name}: fonts 最多 4 项")
    total = 0
    sizes = set()
    need_license = False
    for i, item in enumerate(fonts):
        if not isinstance(item, dict):
            errs.append(f"{root.name}: fonts[{i}] 不是对象")
            continue
        size = item.get("size")
        rel = item.get("file")
        if size not in VALID_SIZES:
            errs.append(f"{root.name}: fonts[{i}].size={size} 非法")
            continue
        if not rel or not isinstance(rel, str):
            errs.append(f"{root.name}: fonts[{i}].file 缺失")
            continue
        if ".." in rel.split("/"):
            errs.append(f"{root.name}: fonts[{i}].file 不能跳出主题目录")
            continue
        if size in sizes:
            errs.append(f"{root.name}: 字号 {size} 重复声明")
        sizes.add(size)
        p = root / rel
        if not p.exists():
            errs.append(f"{root.name}: 缺文件 {rel}")
            continue
        total += p.stat().st_size
        errs += check_font(p, rel)
        if item.get("license"):
            need_license = True
    if total > BUDGET:
        errs.append(f"{root.name}: 字库合计 {total // 1024}KB 超过 {BUDGET // 1024}KB 预算")
    if need_license and not (root / "fonts" / "LICENSE.txt").exists():
        errs.append(f"{root.name}: 声明了 license 但缺 fonts/LICENSE.txt")
    if fonts and not errs:
        print(f"FONT OK {root} ({len(fonts)} size(s), {total // 1024}KB)")
    return errs


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit("Usage: python -B tools/validate_skin_fonts.py SKIN_DIR...")
    bad = 0
    for d in args:
        errs = check_skin(d)
        for e in errs:
            print("FONT ERR", e)
        bad += len(errs)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
