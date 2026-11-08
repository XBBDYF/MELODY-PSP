# 官方皮肤怎么读（抄哪一段）

> 9 套官方皮肤就是最好的教材。按「想学什么」选一套读，不要通读全部。
> 本包 [EXAMPLES/](EXAMPLES/) 里有可直接运行的示例：`paper/`（脚本版）、`ocean/`（配色版）、`eco/`（**省电原型**：整屏 1 块底色 + 4 行文字，无图无动画）。

| 皮肤 | 类型 | 适合学 | 关键文件 |
|---|---|---|---|
| `ocean` | 纯配色 | 最小 `skin.json` 长什么样 | `skins/ocean/skin.json` |
| `mint` / `ruby` / `night` / `daylight` | 纯配色 | 配色变体、深浅两套 | `skins/<id>/skin.json` |
| `metro` | 脚本 | **磁贴网格、动效、选项、音效**（默认皮肤） | `skins/metro/skin.json`、`skin.lua` |
| `metro_day` | 脚本 | 旧白天版兼容入口（与 metro 同源） | 同上 |
| `xmb` | 脚本 | **PSP 横排导航、波浪、页面过渡** | `skins/xmb/skin.json`、`skin.lua`、`xmb_views.lua` |
| `paper` | 脚本 | **纸感排版、静态背景、字体风格** | `skins/paper/skin.json`、`skin.lua` |

---

## 1. 想学「最小可行主题」→ 看 `ocean`

```json
{ "schema": 1, "id": "ocean", "name": "海洋蓝", "dark": true,
  "colors": { "background": "#0B1B2B", "accent": "#3AA0FF" } }
```

只有协议必需字段 + 几个颜色。**你的第一套主题就照这个抄。**

## 2. 想学「磁贴/网格布局」→ 看 `metro`

- `skin.json`：`preferences`（主题颜色、动态磁贴开关）、`assets`（move/confirm/back 音效、唱片图）、`optional_capabilities`；
- `skin.lua`：
  - `tileSets` 定义首页三屏磁贴（名字/坐标/大小/图标/动作）；
  - `neighbor()` 做方向键焦点移动；
  - `beginTurn()` 做页面切换过渡；
  - `skin.draw` 按 `page` 分派到 `pages`；
  - `skin.input` 先处理 `home` 的方向键，再回落原生。

**抄这段**：`tileSets` 的坐标表 + `neighbor()` + `pages.home`。

## 3. 想学「横排导航/过渡动画」→ 看 `xmb`

- 横向分类 + 竖向条目，`xmb_views.lua` 里是视图函数；
- 页面间过渡用 `ui.begin_navigation` / `ui.slide` / `ui.capture_page`；
- 背景波浪用 `ui.wave(t, color)`。

**抄这段**：横向滚动的焦点计算 + `ui.wave` 背景。

## 4. 想学「纸感/静态排版」→ 看 `paper`

- 背景用 `ui.rect` 铺米色，标题用大字距；
- 列表行用 `ui.line` 画细横线；
- 不依赖动效，适合低配/省电。

**抄这段**：`pages.home` 的分栏排版。

## 5. 想学「主题选项」→ 看 `metro`

```json
"preferences": [
  { "id": "color_mode", "label": "主题颜色", "type": "choice", "options": ["黑色", "白色"], "default": 0 },
  { "id": "live_tiles", "label": "磁贴动态翻面", "type": "toggle", "default": 1 }
]
```

```lua
local mode = app.option("color_mode")
local live = app.option("live_tiles")
```

## 6. 想学「主题音效」→ 看 `metro`

```json
"assets": [
  { "id": "move",    "file": "assets/move.wav",    "type": "sound" },
  { "id": "confirm", "file": "assets/confirm.wav", "type": "sound" },
  { "id": "back",    "file": "assets/back.wav",    "type": "sound" }
]
```

```lua
local function sound(id) ui.sound(id) end
```

## 7. 想学「封面/唱片/头像」→ 看 `metro`、`xmb`

```lua
ui.cover(x, y, w, h)                 -- 当前歌曲封面
ui.card_cover(index, x, y, w, h)     -- 列表卡片封面
ui.disc(x, y, size, angle)           -- 唱片
ui.avatar(x, y, w, h)                -- 头像
```

## 8. 想学「搜索页/输入法显示」→ 看 `metro` 的 `search()` 函数

4.1.1 起搜索页有输入法候选、搜索历史、推荐搜索，模型字段：
`ime_active ime_pre ime_candidates ime_selected ime_off search_history search_chips chip_row chip_selected hist_selected assoc chip_off`
布局与交互说明见项目 `docs/SEARCH-UI.md`（不在本包内，属开发者文档）。

---

## 9. 读代码顺序建议

1. 先 `skin.json` —— 弄明白这套主题声明了什么；
2. 再搜 `pages` / `skin.draw` —— 看页面函数怎么分派；
3. 找你要改的那个页面函数（如 `home`/`player`）；
4. 最后看 `input` —— 交互怎么接管、什么时候回落原生。

**不要**一开始就读 1000+ 行 `skin.lua`；按页面名搜索定位最快。
