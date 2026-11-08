# 工具说明

三个脚本都**只读**，不会修改你的主题文件；需要 Python 3.9+。

| 脚本 | 依赖 | 作用 |
|---|---|---|
| `check_skin.py` | 标准库 | 结构校验：JSON/字段/颜色/页面/widget/字体声明 |
| `validate_skin_fonts.py` | 标准库 | 字体文件校验：PSPF 头、边界、升序、预算、LICENSE |
| `make_theme_font.py` | `pillow`（`pip install pillow`） | TTF/OTF → PSPF 子集字库 |

---

## check_skin.py

```bash
python -B TOOLS/check_skin.py skins/my_theme
python -B TOOLS/check_skin.py skins/a skins/b skins/c
```

输出：

- `OK skins/my_theme` — 通过；
- `WARN …` — 不致命（如素材文件不存在、widget 没内容）；
- `ERR …` — 必须修，否则主题会被拒或显示异常。

> 如果系统里有 `lua` / `luac`，还会顺带做一次 Lua 语法检查；没有就跳过。

---

## validate_skin_fonts.py

```bash
python -B TOOLS/validate_skin_fonts.py skins/my_theme
```

只在 `skin.json` 里有 `fonts[]` 时才需要跑。
检查：magic/version、记录长度、码点严格升序、记录越界、合计 ≤1.5MB、单文件 ≤896KB、
声明了 `license` 时 `fonts/LICENSE.txt` 是否存在。

---

## make_theme_font.py

```bash
# 只包含用到的字（推荐）
python -B TOOLS/make_theme_font.py C:/Windows/Fonts/simkai.ttf --size 22 \
    --chars "MELODY开始我的音乐" -o skins/my_theme/fonts/title.bin

# 从文本文件抽取所有字
python -B TOOLS/make_theme_font.py font.ttf --size 16 --text-file titles.txt \
    -o skins/my_theme/fonts/ui.bin

# 按字符集
python -B TOOLS/make_theme_font.py font.ttf --size 12 --charset gb2312 -o x.bin
```

参数：`--size 12|16|22|32`、`--chars`、`--charset`、`--chars-file`、`--text-file`、`-o`。
字库里没有的字会打印告警并跳过。

字体许可：用系统字体/开源字体时，把许可文本放到 `fonts/LICENSE.txt` 并在
`skin.json` 的 `fonts[].license` 里写明。

---

## 需要「真运行时」校验怎么办

本包只带结构校验。完整运行时校验（真正跑 Lua、逐页首帧绘制）在开发者仓库里：

```bash
# 开发者仓库根目录
python -B tools/validate_skin_runtime.py skins/my_theme
```

它需要主机 GCC 和仓库里的 Lua 源码；普通主题作者用 `check_skin.py` 足够。
