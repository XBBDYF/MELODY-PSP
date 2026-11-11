> 2026-11-11新增电源接口：见 [08-POWER-CONTROLS.md](08-POWER-CONTROLS.md)。原有字段/页面不变；新增 power_* 模型、三个动作以及system_*主题选项。默认不自动套用主题电源设置。

# 参考：字段与 API 全表

> 以 MELODY 4.1.1 的 `src/skin_runtime.c` 为准。表中没列的函数/字段不要用。
> 参数顺序不确定时，去 `skins/metro/skin.lua`、`skins/xmb/skin.lua` 搜同名调用。

---

## 1. `skin.json` 字段

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `schema` | number | 是 | 固定 `1` |
| `id` | string | 是 | ≤32，`a-zA-Z0-9_-`，必须与目录名一致 |
| `name` | string | 是 | 皮肤列表显示名 |
| `author` | string | 否 | 作者 |
| `version` | string | 否 | 主题版本（和程序版本无关） |
| `description` | string | 否 | 一句话说明 |
| `dark` | bool | 否 | true=深色默认板，false=浅色 |
| `colors` | object | 否 | 15 个色键，见 §2 |
| `script` | string | 否 | 写 `"skin.lua"`；不写则纯 widget/原生 |
| `chrome` | object | 否 | `mini_player` / `status` / `transition`，`false` = 隐藏原生部件 |
| `pages` | object | 否 | `pages.<页面名>.widgets[]`，见 §3 |
| `assets` | array | 否 | `{id,file,type}`，`type` = `image` / `sound` |
| `preferences` | array | 否 | 主题选项，见 §4 |
| `fonts` | array | 否 | 主题字体，见 §5 |
| `min_app_version` | string | 否 | 例如 `"4.1.1"` |
| `optional_capabilities` | array | 否 | 能力声明，旧程序会忽略 |

> 字符串长度上限：`id` 32、`name`/`author` 48、`description` 160、`version` 16。超长会被判为无效。

---

## 2. 颜色键（只有这 15 个）

| 键 | 用途 |
|---|---|
| `background` | 页面底色 |
| `panel` | 面板/卡片底 |
| `panel2` | 次级面板（列表行、输入框） |
| `text` | 主文字 |
| `secondary` | 次要文字 |
| `muted` | 弱提示文字 |
| `line` | 分隔线/边框 |
| `accent` | 强调色（高亮、选中） |
| `hot` | 更热的强调（播放键、危险操作） |
| `row` | 列表行底 |
| `white` | 纯白（在深色底上画符号） |
| `black` | 纯黑 |
| `success` | 成功/已下载 |
| `warning` | 警告/失败 |
| `shadow` | 阴影 |

写错键名 = 主题校验失败（不会静默忽略）。

---

## 3. widget（不用 Lua 的排版）

```json
"pages": {
  "home": {
    "widgets": [
      { "x": 20, "y": 20, "width": 200, "height": 80, "type": "rect", "color": "#223344" },
      { "x": 30, "y": 30, "width": 180, "height": 24, "type": "text", "text": "标题", "font": 2, "color": "#FFFFFF" },
      { "x": 30, "y": 60, "width": 180, "height": 16, "type": "text", "bind": "song", "font": 0, "color": "#22CC88" }
    ]
  }
}
```

| 字段 | 说明 |
|---|---|
| `x` `y` `width` `height` | 0..960（越界截断） |
| `font` | 0=12px，1=16px，2=22px，3=32px |
| `type` | `rect` / `text` |
| `text` | 静态文本 |
| `bind` | 绑定模型字段名（见 §6），与 `text` 二选一 |
| `color` | 颜色 |
| `action` / `argument` | 预留（当前版本不保证生效） |

widget 每页最多 32 个；超出会被判无效。

---

## 4. preferences（主题选项）

```json
"preferences": [
  { "id": "color_mode", "label": "配色", "type": "choice", "options": ["黑", "白"], "default": 0 },
  { "id": "live_tiles", "label": "动态磁贴", "type": "toggle", "default": 1 }
]
```

Lua：`app.option("color_mode")` → `0`/`1`；`app.option("live_tiles")` → `0`/`1`。

---

## 5. fonts（主题自带字体）

```json
"fonts": [
  { "size": 22, "file": "fonts/title.bin", "name": "Noto Serif", "license": "OFL-1.1" }
]
```

- `size` 只能是 12 / 16 / 22 / 32；
- 最多 4 项；合计 ≤ 1.5MB，单文件 ≤ 896KB；
- 缺字自动回退内置字库；
- 生成与校验见 [04-REFERENCE-FONTS.md](04-REFERENCE-FONTS.md)。

---

## 6. 模型 `m`（只读）

字段名可用于 `bind`，也可在 Lua 里直接读。**全部只读**，字段可能为 `nil`（旧版程序）。

### 基本
`page width height frame_dt index total percent selected label value group`

### 播放
`song artist song_id paused position_ms duration_ms volume audio_level audio_state play_mode liked quality`
`buffered_ms buffering show_speed show_memory memory_used_kb memory_free_kb disc_player menu_open menu_selected`

### 列表 / 歌曲 / 下载
`list_kind list_title songs cards subtitle tracks kind id name bytes cache playable fee`
`downloads download_count download_tab download_select_mode download_selected download_selected_count download_marked pending_downloads total_bytes state percent`

### 搜索（4.1.1+）
`query query_len search_edit search_state search_kind search_category`
`suggestions suggest_focused suggest_selected kb_row kb_col`
`ime_mode ime_active ime_pre ime_candidates ime_selected ime_off assoc`
`search_history search_chips chip_row chip_selected hist_selected chip_off`

### 首页 / 我的 / 网格
`home_selected mine_selected mine grid_selected category_selected category_focused`

### 缓存 / 本地
`cache_count cache_bytes cache_percent cache_selected cache_tab local_count local_files local_selected`

### 设置
`setting_selected settings`

### 网络 / 时间 / 存储
`network_kbps wlan_enabled wifi_name free_storage_mb online battery clock`

### 歌词
`lyric_current lyric_scroll lyric_translation lyrics text translation current ms`

### 账号 / 评论 / 其他
`logged_in login_name qr_state user likes replies comments daily_cover_available daily_song daily_artist xmb_navigation find`

> 字段集合以 `src/skin_runtime.c` 的 `push_model()` 为准；本包与 4.1.1 同步。
> 取值一律兜底：`(m.song or "")`、`(m.list_kind or 0)`、`(m.songs or {})`。

## 7. `ui.*` 绘制与资源

| 函数 | 说明 |
|---|---|
| `ui.rect(x,y,w,h,color)` | 实心矩形 |
| `ui.round(x,y,w,h,r,color)` | 圆角矩形 |
| `ui.circle(x,y,r,color)` | 圆 |
| `ui.line(x1,y1,x2,y2,color,width?)` | 线（width 1..8） |
| `ui.text(x,y,text,font?,color?)` | 画文字；font 0..3 |
| `ui.label(x,y,w,text,font?,color?)` | 定宽文字，超宽省略 |
| `ui.text_width(text,font?)` | 返回像素宽 |
| `ui.icon(name,x,y,size,color)` | 内置图标 |
| `ui.cover(x,y,w,h)` | 当前歌曲封面，返回 bool |
| `ui.card_cover(index,x,y,w,h)` | 卡片封面（歌单/专辑列表） |
| `ui.tile_cover(source,index,x,y,w,h)` | 磁贴封面，`source` = 数据源名 |
| `ui.avatar(x,y,w,h)` | 头像 |
| `ui.image(asset_id,x,y,w,h)` | 主题资源图片 |
| `ui.qr(x,y,size)` | 当前登录二维码 |
| `ui.gradient(x,y,w,h,top,bottom,r?,horiz?)` | 渐变（可圆角/横向） |
| `ui.disc(x,y,size,angle)` | 唱片（angle 弧度） |
| `ui.wave(t,color)` | 音乐波浪，`t` = 秒 |
| `ui.music_fx(style,t,level)` | 音乐动效，style 0..2，level 0..100 |
| `ui.lyric_text(x,y,w,text,font,base_color,active_color,index,offset?)` | 卡拉 OK 歌词行 |
| `ui.overlay(x,y,w,h,color,alpha?)` | 半透明遮罩，alpha 0..255（默认 160） |
| `ui.clip(x,y,right,bottom)` / `ui.unclip()` | 裁剪区域 |
| `ui.module(name,x?,y?,w?,h?)` | 画原生模块（如迷你播放器） |
| `ui.slide(dx,top,bottom)` | 页面滑动过渡 |
| `ui.unfold(amount)` | 展开动画 0..1 |
| `ui.capture_page()` / `ui.release_page()` | 页面过渡截图/释放 |
| `ui.sound(asset_id)` | 播放主题音效，返回 bool |
| `ui.begin_navigation()` | 标记原生导航开始（过渡用） |

---

## 8. `app.*`

| 函数 | 说明 |
|---|---|
| `app.dispatch(action, arg?)` | 触发原生动作 |
| `app.option(id)` | 读主题选项 |
| `app.model()` | 取一份模型表（多数情况直接用回调里的 `m`） |

常用 `dispatch` 动作：`home`、`mine`、`settings`、`search`、`skin`、`open`（arg=页面）、`player`、`downloads`、`cache`、`queue_toggle`、`toggle_translation`、`grid_select`（arg=索引）、`category_select`（arg=分类）。

> 不同版本动作集合可能变化；用之前先在官方皮肤里搜一下是否有同样调用。

---

## 9. 输入 `k`

| 字段 | 说明 |
|---|---|
| `pressed` | 本帧刚按下 |
| `held` | 持续按住 |
| `up down left right` | 方向 |
| `confirm` | ○ |
| `back` | × |
| `square` | □ |
| `triangle` | △ |
| `select` | SELECT |
| `l r` | L / R |
| `start` | START |

```lua
if (k.pressed & k.confirm) ~= 0 then ... end
```

---

## 10. 图标名（`ui.icon`）

常用：`home` `search` `list` `music` `play` `pause` `heart` `download` `cache` `settings` `wifi` `battery` `clock` `back` `next` `prev` `disc` `mic` `star` `user` `check` `close` `plus` `minus` `shuffle` `repeat`

> 名字写错不会崩，只是画不出来；以官方皮肤用到的为准。
