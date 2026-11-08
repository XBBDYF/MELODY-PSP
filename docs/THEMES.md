# 皮肤与主题

MELODY 的界面可以用 Lua 皮肤完全重画。程序本身提供绘制原语、输入事件与数据模型，
皮肤脚本决定「长什么样」「怎么响应」。

`skin_release/melody/skins/` 下有 9 套可直接用的示例：`metro`、`metro_day`、`xmb`、`paper`、`mint`、
`ocean`、`ruby`、`night`、`daylight`。

---

## 1. 目录结构

```
skin_release/melody/skins/<id>/
├── skin.json      元数据：名称、作者、颜色、脚本、资源、页面开关
├── skin.lua       绘制与输入（必需）
├── preview.png    主题列表里的预览图（可选
└── assets/        图片、音效等资源（按 skin.json 声明）
```

### skin.json

```json
{
  "schema": 1,
  "id": "metro",
  "name": "Metro · Windows Phone",
  "author": "洗不白de衣服 / XBBDYF",
  "version": "5.7",
  "description": "黑白配色合一，可即时切换颜色与动态磁贴",
  "dark": true,
  "colors": {
    "background": "#080808", "panel": "#080808", "text": "#FFFFFF",
    "secondary": "#BBBBBB", "muted": "#969696", "line": "#3E3E3E",
    "accent": "#0078D7", "hot": "#0078D7", "panel2": "#171717",
    "row": "#292930", "white": "#FFFFFF", "black": "#000000",
    "success": "#54D49B", "warning": "#F5A957", "shadow": "#000000"
  },
  "script": "skin.lua",
  "chrome": { "mini_player": false, "status": false, "transition": false },
  "assets": [
    { "id": "move", "file": "assets/move.wav", "type": "sound" }
  ]
}
```

- `min_app_version` 可选；高于当前程序版本时该皮肤会被跳过（用于渐进升级）。
- `assets` 里声明的文件会预加载到内存/播放器，脚本用 `ui.sound("move")` 播放。

---

## 2. 页面

程序内部页面名（`skin_runtime.c` 的 `page_names`）：

`boot wifi home songs player lyrics search downloads cache settings about
account grid square comments mine video sources skins`

皮肤按需实现，未实现的页面自动回退到原生界面。

---

## 3. host API

初始化时注册两个全局表，并移除危险函数（`dofile`/`load`/`pcall`/`setmetatable`/`print` 等），
`string.find` 被替换成有限工作量的字面量版本（带 1MB 总预算）。

### `ui.*` 绘制与控件

| 函数 | 说明 |
|---|---|
| `ui.rect(x, y, w, h, color)` | 填充矩形 |
| `ui.round(x, y, w, h, r, color)` | 圆角矩形 |
| `ui.circle(x, y, r, color)` | 圆 |
| `ui.line(x1, y1, x2, y2, color, width)` | 线 |
| `ui.text(x, y, w, s, align, color)` | 文本（`align` 0 左 / 1 居中 / 2 右） |
| `ui.text_width(s, font)` | 文本宽度 |
| `ui.label(x, y, w, h, s, color, font)` | 单行标签 |
| `ui.image(x, y, w, h, name)` | 画已声明的图片资源 |
| `ui.cover(x, y, w, h, url)` | 画封面（异步下载，自动缓存） |
| `ui.tile_cover` / `ui.card_cover` | 磁贴/卡片封面（无图时返回 false，可自绘占位） |
| `ui.avatar(x, y, r, url)` | 圆形头像 |
| `ui.gradient(x, y, w, h, c1, c2, dir)` | 渐变 |
| `ui.disc(x, y, r, angle)` | 唱片 |
| `ui.wave(x, y, w, h, color, seed)` | 波形 |
| `ui.lyric_text(...)` | 歌词排版（含翻译行） |
| `ui.qr(x, y, size, text)` | 二维码 |
| `ui.sound(id)` | 播放皮肤音效 |
| `ui.clip(x, y, w, h)` / `ui.unclip()` | 裁剪区域 |
| `ui.begin_navigation` / `ui.page_fold` / `ui.capture_page` / `ui.page_turn` / `ui.release_page` | 翻页转场 |
| `ui.overlay` / `ui.unfold` / `ui.slide` | 覆盖层与展开/滑动动画 |
| `ui.module(name, ...)` | 调用可选的 C 模块（音量、唱片、选项面板等） |

### `app.*`

| 函数 | 说明 |
|---|---|
| `app.model()` | 取当前页面的数据模型（见下） |
| `app.option(id, fallback)` | 读皮肤自己的选项值（`skin.json` 的 options / 设置页调色） |
| `app.dispatch(action, arg)` | 向程序发动作，例如 `"play"`、`"next"`、`"open_settings"` |

### 模型字段（节选）

`app.model()` 返回的表包含：`page/width/height/frame_dt/selected/memory_*`、
播放状态（`song/artist/song_id/position_ms/duration_ms/paused/buffered_ms/audio_level/play_mode/liked`）、
列表（`songs[] = {index,id,name,artist,playable,fee,download_marked}`、`list_title/list_kind`）、
卡片（`cards[]`）、歌词（`lyric_current/lyric_scroll/lyrics[]`）、
评论（`comments[]`）、下载队列（`downloads[]`）、缓存（`cache[]`）、
网络（`network_kbps/wlan_enabled/wifi_name/online`）、
状态栏（`battery/clock/show_speed/show_memory`）、
设置项（`settings[] = {index,label,value,group}`）、搜索（`query/query_len/search_kind/search_state/suggestions[]`）。

> 列表只给「选中项附近」的窗口（歌曲 24 条、评论 12 条、设置 7 条、下载全部），
> 这是为了控制每帧建表的内存与时间开销。

---

## 4. 最小示例

```lua
-- skin_release/melody/skins/hello/skin.lua
local function box(x, y, w, h, c) ui.rect(x, y, w, h, c) end

function draw(page, m)
    box(0, 0, m.width, m.height, "#101020")
    ui.text(24, 20, m.width - 48, "MELODY", 0, "#FFFFFF")
    if m.song ~= "" then
        ui.text(24, 48, m.width - 48, m.song, 0, "#54D49B")
        ui.text(24, 68, m.width - 48, m.artist, 0, "#969696")
    end
    box(16, m.height - 40, m.width - 32, 4, "#C51C30")
end

function input(page, pressed, held, dt)
    if pressed == "start" then app.dispatch("play") end
end
```

---

## 5. 校验与调试

```bash
python -B tools/validate_skin_runtime.py skin_release/melody/skins/hello
```

校验内容：`skin.json` 必需字段、颜色格式、Lua 语法、必需页面函数、资源文件存在。
程序侧在主题页显示安装/加载失败原因（`skin_catalog_install_error()`、`skin_runtime_error()`），
日志里对应 `skin io:` / `skin apply:` 前缀。

修改已有主题时，`metro` 与 `metro_day` 共用同一份脚本（只换颜色模式），
改完记得同步两个目录；`tools/skin_templates/metro.lua` 是模板源文件。


