"""MELODY 主题结构校验（离线、只读、无第三方依赖）。

用法:
    python -B TOOLS/check_skin.py skins/my_theme
    python -B TOOLS/check_skin.py skins/a skins/b skins/c

检查项:
  - skin.json 可解析、必填字段、id 合法且与目录名一致
  - colors 只用合法键、值为 #RRGGBB
  - pages 页面名合法、widget 数量与坐标范围
  - assets / preferences / fonts 结构
  - 若系统里有 lua/luac，再做一次语法检查（没有就跳过）

注意: 这是结构校验，不做 Lua 运行时验证；运行时验证需要在开发者仓库里跑
validate_skin_runtime.py。
"""
from pathlib import Path
import json
import re
import shutil
import subprocess
import sys

SCHEMA = 1
PAGES = {
    "boot", "wifi", "home", "songs", "player", "lyrics", "search", "downloads",
    "cache", "settings", "about", "account", "grid", "square", "comments",
    "mine", "video", "sources", "skins",
}
COLORS = {
    "background", "panel", "panel2", "text", "secondary", "muted", "line",
    "accent", "hot", "row", "white", "black", "success", "warning", "shadow",
}
FONT_SIZES = {12, 16, 22, 32}
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
HEX_RE = re.compile(r"^#[0-9A-Fa-f]{6}$")
MAX_WIDGETS = 32


def check_color(value):
    return isinstance(value, str) and bool(HEX_RE.match(value))


def check_skin(root):
    root = Path(root)
    errs = []
    warnings = []

    manifest_path = root / "skin.json"
    if not manifest_path.exists():
        return [f"{root.name}: 缺少 skin.json"], warnings
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as exc:
        return [f"{root.name}: skin.json 解析失败（{exc}）"], warnings
    if not isinstance(data, dict):
        return [f"{root.name}: skin.json 顶层必须是对象"], warnings

    # 必填
    if data.get("schema") != SCHEMA:
        errs.append(f"{root.name}: schema 必须是 {SCHEMA}")
    sid = data.get("id")
    if not isinstance(sid, str) or not ID_RE.match(sid or ""):
        errs.append(f"{root.name}: id 非法（1..32，仅 a-zA-Z0-9_-）")
    elif sid != root.name:
        errs.append(f"{root.name}: id={sid!r} 与目录名不一致")
    if not isinstance(data.get("name"), str) or not data.get("name", "").strip():
        errs.append(f"{root.name}: 缺少 name")

    for key, limit in (("name", 48), ("author", 48), ("description", 160), ("version", 16)):
        value = data.get(key)
        if value is None:
            continue
        if not isinstance(value, str):
            errs.append(f"{root.name}: {key} 必须是字符串")
        elif len(value.encode("utf-8")) > limit:
            errs.append(f"{root.name}: {key} 超过 {limit} 字节")

    if "dark" in data and not isinstance(data["dark"], bool):
        errs.append(f"{root.name}: dark 必须是 bool")

    # colors
    colors = data.get("colors")
    if colors is not None:
        if not isinstance(colors, dict):
            errs.append(f"{root.name}: colors 必须是对象")
        else:
            for key, value in colors.items():
                if key not in COLORS:
                    errs.append(f"{root.name}: colors.{key} 不是合法颜色键")
                elif not check_color(value):
                    errs.append(f"{root.name}: colors.{key}={value!r} 不是 #RRGGBB")

    # script
    script = data.get("script")
    if script is not None:
        if not isinstance(script, str):
            errs.append(f"{root.name}: script 必须是字符串")
        elif (root / script).exists() is False:
            errs.append(f"{root.name}: script={script!r} 文件不存在")
        elif not script.endswith(".lua"):
            warnings.append(f"{root.name}: script 通常指向 .lua 文件")

    # chrome
    chrome = data.get("chrome")
    if chrome is not None:
        if not isinstance(chrome, dict):
            errs.append(f"{root.name}: chrome 必须是对象")
        else:
            for key, value in chrome.items():
                if key not in ("mini_player", "status", "transition"):
                    warnings.append(f"{root.name}: chrome.{key} 未知，会被忽略")
                if not isinstance(value, bool):
                    errs.append(f"{root.name}: chrome.{key} 必须是 bool")

    # pages / widgets
    pages = data.get("pages")
    if pages is not None:
        if not isinstance(pages, dict):
            errs.append(f"{root.name}: pages 必须是对象")
        else:
            for page_name, page in pages.items():
                if page_name not in PAGES:
                    errs.append(f"{root.name}: pages.{page_name} 不是合法页面名")
                    continue
                widgets = (page or {}).get("widgets")
                if widgets is None:
                    continue
                if not isinstance(widgets, list):
                    errs.append(f"{root.name}: pages.{page_name}.widgets 必须是数组")
                    continue
                if len(widgets) > MAX_WIDGETS:
                    errs.append(f"{root.name}: pages.{page_name} widget 超过 {MAX_WIDGETS} 个")
                for i, w in enumerate(widgets):
                    tag = f"{root.name}: pages.{page_name}.widgets[{i}]"
                    if not isinstance(w, dict):
                        errs.append(f"{tag} 必须是对象")
                        continue
                    if w.get("type") not in ("rect", "text"):
                        errs.append(f"{tag}.type 只能是 rect/text")
                    for key, hi in (("x", 960), ("y", 544), ("width", 960), ("height", 544)):
                        v = w.get(key)
                        if v is not None and (not isinstance(v, int) or v < 0 or v > hi):
                            errs.append(f"{tag}.{key}={v!r} 超出 0..{hi}")
                    f = w.get("font")
                    if f is not None and f not in (0, 1, 2, 3):
                        errs.append(f"{tag}.font 只能是 0..3")
                    if "color" in w and not check_color(w["color"]):
                        errs.append(f"{tag}.color 不是 #RRGGBB")
                    if w.get("type") == "text" and not w.get("text") and not w.get("bind"):
                        warnings.append(f"{tag} 没有 text/bind，什么都不会显示")

    # assets
    assets = data.get("assets")
    if assets is not None:
        if not isinstance(assets, list):
            errs.append(f"{root.name}: assets 必须是数组")
        else:
            for i, a in enumerate(assets):
                tag = f"{root.name}: assets[{i}]"
                if not isinstance(a, dict):
                    errs.append(f"{tag} 必须是对象")
                    continue
                if not isinstance(a.get("id"), str) or not a["id"]:
                    errs.append(f"{tag}.id 缺失")
                if a.get("type") not in ("image", "sound"):
                    errs.append(f"{tag}.type 只能是 image/sound")
                f = a.get("file")
                if not isinstance(f, str):
                    errs.append(f"{tag}.file 缺失")
                elif ".." in f.split("/"):
                    errs.append(f"{tag}.file 不能跳出主题目录")
                elif not (root / f).exists():
                    warnings.append(f"{tag}.file={f!r} 不存在（运行时只会忽略）")

    # preferences
    prefs = data.get("preferences")
    if prefs is not None:
        if not isinstance(prefs, list):
            errs.append(f"{root.name}: preferences 必须是数组")
        else:
            for i, p in enumerate(prefs):
                tag = f"{root.name}: preferences[{i}]"
                if not isinstance(p, dict):
                    errs.append(f"{tag} 必须是对象")
                    continue
                if not isinstance(p.get("id"), str) or not p["id"]:
                    errs.append(f"{tag}.id 缺失")
                if p.get("type") not in ("choice", "toggle"):
                    errs.append(f"{tag}.type 只能是 choice/toggle")
                if p.get("type") == "choice":
                    options = p.get("options")
                    if not isinstance(options, list) or not options:
                        errs.append(f"{tag}.options 必须是非空数组")
                    elif not isinstance(p.get("default", 0), int) or not 0 <= p.get("default", 0) < len(options):
                        errs.append(f"{tag}.default 越界")

    # fonts
    fonts = data.get("fonts")
    if fonts is not None:
        if not isinstance(fonts, list):
            errs.append(f"{root.name}: fonts 必须是数组")
        elif len(fonts) > 4:
            errs.append(f"{root.name}: fonts 最多 4 项")
        else:
            seen = set()
            total = 0
            for i, f in enumerate(fonts):
                tag = f"{root.name}: fonts[{i}]"
                if not isinstance(f, dict):
                    errs.append(f"{tag} 必须是对象")
                    continue
                if f.get("size") not in FONT_SIZES:
                    errs.append(f"{tag}.size 只能是 12/16/22/32")
                elif f["size"] in seen:
                    errs.append(f"{tag}.size 重复")
                else:
                    seen.add(f["size"])
                rel = f.get("file")
                if not isinstance(rel, str):
                    errs.append(f"{tag}.file 缺失")
                elif ".." in rel.split("/"):
                    errs.append(f"{tag}.file 不能跳出主题目录")
                elif not (root / rel).exists():
                    errs.append(f"{tag}.file={rel!r} 不存在")
                else:
                    total += (root / rel).stat().st_size
            if total > 1536 * 1024:
                errs.append(f"{root.name}: 字体合计超过 1.5MB")

    # Lua 语法（有 luac/lua 才做）
    lua = next((shutil.which(n) for n in ("luac5.4", "luac", "lua5.4", "lua") if shutil.which(n)), None)
    if script and (root / script).exists() and lua:
        cmd = [lua, "-p", str(root / script)] if "luac" in Path(lua).name else [lua, "-e", f"assert(loadfile([[{root / script}]]))"]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            errs.append(f"{root.name}: Lua 语法错误：{(proc.stderr or proc.stdout).strip()[:200]}")

    return errs, warnings


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit("用法: python -B TOOLS/check_skin.py skins/<id> [更多目录]")
    failed = 0
    for d in args:
        errs, warnings = check_skin(d)
        for w in warnings:
            print("WARN", w)
        for e in errs:
            print("ERR ", e)
        if not errs:
            print("OK   ", d)
        failed += len(errs)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
