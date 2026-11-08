# 人类教程：做一套 MELODY 主题

> 读完这篇能独立完成：配色主题 → widget 排版 → 脚本主题 → 字体 → 打包分发。
> 需要的话再查 [03-REFERENCE-API.md](03-REFERENCE-API.md)（字段与函数全表）。

---

## 1. 主题是怎么被加载的

1. 程序启动时扫描 `./skins/<任意ID>/skin.json`（`./` 就是 EBOOT 所在目录）；
2. 读取 `id/name/author/version/description/dark/colors`；
3. 如果 `script: "skin.lua"` 存在 → 启动 Lua 5.4 VM，执行 `skin.lua`；
4. `skin.lua` 返回一个 table：`return { pages = {...}, input = function(...) ... end }`；
5. 绘制时程序调用 `pages[当前页面名](t, m)`，`m` 是只读模型（状态快照）；
6. 输入时先问脚本 `input(page, t, dt, m, k)`，返回 `true` 表示脚本吃掉了这次输入，否则交给原生逻辑。

**页面名**（可用于 `pages` 的 key）：
`boot wifi home songs player lyrics search downloads cache settings about account grid square comments mine video sources skins`

---

## 2. 两种主题

### A. 纯配色主题（不需要 Lua）
只有 `skin.json`。所有页面用原生界面，只换颜色/明暗。

### B. 脚本主题（需要 `skin.lua`）
自己画页面、自己处理输入。可以只覆盖部分页面（没写的页面继续用原生）。

> 大多数人的做法：从 `TEMPLATES/minimal/skin.lua` 或官方 `metro`/`xmb` 改。

---

## 3. 从零做一套配色主题

1. 新建 `skins/my_night/`；
2. 写 `skin.json`（见 [README](../README.md) §2 的模板）；
3. 放一张 `preview.png`（480×272，可选，皮肤列表预览）；
4. 拷到 `ms0:/PSP/GAME/MELODY/skins/my_night/`；
5. 进「皮肤」页选择它；
6. 想微调就改颜色 → 保存 → 重启程序（配色是启动时读取的）。

**注意**：`dark` 只影响"没写的颜色"的默认值；写了 `colors` 就以你写的为准。

---

## 4. 用 widget 排版（不会 Lua 也能改布局）

`skin.json` 里可以给每个页面摆若干矩形/文字块，程序用原生字体绘制：

```json
{
  "schema": 1,
  "id": "my_widget",
  "name": "方块面板",
  "author": "你",
  "version": "1.0",
  "dark": true,
  "colors": { "background": "#101010", "text": "#FFFFFF", "accent": "#22CC88" },
  "pages": {
    "home": {
      "widgets": [
        { "x": 20, "y": 100, "width": 200, "height": 60, "type": "rect", "color": "#223344" },
        { "x": 30, "y": 110, "width": 180, "height": 24, "type": "text", "text": "我的音乐", "font": 2, "color": "#FFFFFF" },
        { "x": 30, "y": 140, "width": 180, "height": 16, "type": "text", "bind": "song", "font": 0, "color": "#22CC88" }
      ]
    }
  }
}
```

- `type`：`rect` / `text`（其它值会被忽略）；
- `bind`：把模型字段显示出来（如 `song`、`artist`、`list_title`、`volume`）；
- `font`：`0`=12px，`1`=16px，`2`=22px，`3`=32px；
- 坐标基于 480×272，越界会被截断。

> widget 只做静态块；滚动、动画、复杂交互仍要 Lua。

---

## 5. 写脚本主题

最小骨架（`skin.lua`）：

```lua
local C = {
  bg = "#101010", fg = "#FFFFFF", dim = "#8899AA",
  accent = "#22CC88", panel = "#1B1B1B",
}

local function home(t, m)
  ui.rect(0, 0, 480, 272, C.bg)
  ui.text(24, 40, "MELODY", 2, C.accent)
  ui.text(24, 90, (m.song ~= "" and m.song) or "未播放", 1, C.fg)
  ui.text(24, 120, m.artist or "", 0, C.dim)
  ui.label(24, 200, 432, "○ 播放   △ 搜索   □ 设置", 0, C.dim)
end

local skin = {
  pages = { home = home },
  input = function(page, t, dt, m, k)
    return false      -- false = 交回原生处理
  end,
}
return skin
```

要点：

1. `ui.rect / ui.text / ui.label / ui.icon / ui.round / ui.line` 是主要绘制函数（见 API 表）；
2. 颜色可以是 `"#RRGGBB"` 或 `C.xxx` 自定义；
3. `m` 是**只读**模型；不要修改它，也不会生效；
4. `input` 里先判断页面名；返回 `true` 就完全接管这次按键；
5. 不要在绘制函数里做重活（每帧都会调用）；需要缓存就缓存到脚本局部变量。

---

## 6. 输入处理

```lua
input = function(page, t, dt, m, k)
  if page ~= "home" then return false end
  if k.pressed & k.confirm ~= 0 then
    app.dispatch("open", "player")    -- 交给原生动作
    return true
  end
  if k.pressed & k.back ~= 0 then
    app.dispatch("home")
    return true
  end
  return false
end
```

- `k` 里的键：`up down left right confirm(circle) back(cross) square triangle select l r start`；
- `k.pressed` 是"刚按下"，`k.held` 是"按住"（长按连发用 `repeatDirection` 之类的本地函数，参考 metro）；
- `app.dispatch(action, arg)` 触发原生动作（`home`、`open`、`search`、`settings`、`player` 等，见 API 表）。

---

## 7. 资源（图片 / 音效）

`skin.json`：

```json
"assets": [
  { "id": "bg",      "file": "assets/bg.png",      "type": "image" },
  { "id": "confirm", "file": "assets/confirm.wav", "type": "sound" }
]
```

Lua：

```lua
ui.image("bg", 0, 0, 480, 272)
ui.sound("confirm")
```

限制：PNG 建议不超过 480×272（大图会占内存）；WAV 用 16-bit 单声道 22050/44100Hz，单个 < 100KB 为宜。

---

## 8. 主题选项（preferences）

```json
"preferences": [
  { "id": "color_mode", "label": "配色", "type": "choice", "options": ["黑", "白"], "default": 0 },
  { "id": "live_tiles", "label": "动态磁贴", "type": "toggle", "default": 1 }
]
```

Lua 读取：

```lua
local mode = app.option("color_mode")    -- 0 / 1
local live = app.option("live_tiles")    -- 0 / 1
```

选项会显示在「设置 → 主题选项」里。

---

## 9. 主题自带字体（可选）

1. 用 `TOOLS/make_theme_font.py` 从 TTF 生成子集（只含主题用到的字）：
   ```bash
   python -B TOOLS/make_theme_font.py noto.ttf --size 22 --chars "MELODY开始我的音乐" -o skins/my/fonts/title.bin
   ```
2. `skin.json` 声明：
   ```json
   "fonts": [ { "size": 22, "file": "fonts/title.bin", "name": "Noto", "license": "OFL-1.1" } ]
   ```
3. 缺字自动回退内置字库，所以子集不用全；
4. 合计不超过 1.5MB，单文件 ≤ 896KB；
5. 详情见 [04-REFERENCE-FONTS.md](04-REFERENCE-FONTS.md)。

---

## 10. 打包与分发

```text
my_theme.zip
└─ skins/my_theme/
   ├─ skin.json
   ├─ skin.lua        （有脚本时）
   ├─ preview.png
   ├─ fonts/…         （有字体时）
   ├─ assets/…
   └─ CREDITS.txt     （引用了第三方素材时）
```

- 只发主题目录，不要带 `skin-prefs/`、缓存、`data.txt`；
- 群里发 zip；文件名用 `<主题ID>.zip`；
- 发布前跑一遍 [06-CHECKLIST.md](06-CHECKLIST.md)。

---

## 11. 命令速查

```bash
python -B TOOLS/validate_skin_runtime.py skins/my_theme    # 结构 + Lua 运行
python -B TOOLS/validate_skin_fonts.py   skins/my_theme    # 字体（有 fonts[] 时）
python -B TOOLS/make_theme_font.py font.ttf --size 22 --chars "文本" -o skins/my_theme/fonts/x.bin
```

## 12. 轻量 / 省电主题

- **深色不等于省电**：PSP 是 LCD，背光常亮，黑色像素照样耗电；省电来自「每帧少干活」；
- 少画：整屏底色用 1 个 `ui.rect` 替代渐变/多层叠加；每页文字控制在几行；
- 不逐帧动画：`t` 不参与绘制；不做每帧变化的效果；
- 不带资源：不引用图片/字体/音效，少一次存储读取；
- 需要滚动/动画的页面交给原生（不写该页面即可回落）；
- 参考 `EXAMPLES/eco/`。

> 主题能影响的只有「绘制量」；时钟、背光、Wi-Fi、音频解码都不在主题 API 里。
