# 给 AI 的工作说明（主题制作 / 修改）

> 你是主题助手。目标：产出**能被 MELODY 直接加载、不会崩、校验通过**的主题文件。
> 先读本页，再读 [03-REFERENCE-API.md](03-REFERENCE-API.md) 查字段/函数；不要凭记忆编 API。

---

## 0. 你面对的是什么

- PSP 原生应用，画面固定 **480×272**，字体是点阵（12/16/22/32 四档）；
- 主题是 **Lua 5.4（32 位整数）** 脚本 + 一个 JSON 清单；
- 主题运行在**受限沙箱**：没有 `io`、`os`、`require`、`loadfile`、网络、文件系统，不能调用 C；
- 主题只能**绘制和改界面**，不能改播放/网络/下载逻辑；
- 主题文件放在 `./skins/<id>/`（`./` = EBOOT 目录）。

---

## 1. 可改与不可改（硬边界）

| 可以做 | 不要做 |
|---|---|
| 新建 `skins/<id>/`（`skin.json`、`skin.lua`、`assets/`、`preview.png`） | 改 `src/` 里的 C 代码（主题包不包含它） |
| 用 `ui.*` 绘制、用 `app.dispatch` 触发原生动作 | 调 `io.*` / `os.*` / `require` / `dofile` / `package` |
| 用 `m` 读状态 | 写 `m`（只读，写了也没用） |
| 声明 `colors` / `preferences` / `fonts` / `assets` | 写死 `ms0:/PSP/GAME/MELODY/` 绝对路径 |
| 覆盖部分页面（其余回落原生） | 声称"支持所有页面"却不验证 |
| 复制官方皮肤再改（记得改 `id`） | 直接覆盖官方 `metro`/`xmb` 后仍用原 ID 冒充分发 |

> 若任务要求"改程序功能"（网络、播放、下载、搜索算法），说明超出主题范围，只能改界面表现。

---

## 2. 标准工作流

1. **先看现状**：读 `skins/<相近主题>/skin.json`（结构）、`skin.lua`（画法与输入）；
2. **定方案**：只配色？ + widget？ + 脚本页面？列出要覆盖的页面名；
3. **建目录**：从 `TEMPLATES/minimal/` 复制，改 `id`（小写字母/数字/`_`/`-`，≤32 字符）；
4. **写文件**（UTF-8 无 BOM；JSON 不能有注释和尾逗号）；
5. **自检**：
   ```bash
   python -B TOOLS/validate_skin_runtime.py skins/<id>
   python -B TOOLS/validate_skin_fonts.py   skins/<id>     # 有 fonts[] 时
   ```
6. **报告**：改了哪些文件、验证输出、未验证项（真机/PPSSPP 截图）分开写。

---

## 3. 语言与格式硬约束

- **UTF-8 无 BOM**；中文直接写，不要转义；
- JSON：`schema: 1` 固定；字符串不要超过字段长度（`id` ≤32、`name`/`author` ≤48、`description` ≤160、`version` ≤16）；
- 颜色只接受 `"#RRGGBB"`；可用的颜色键见 API 文档的 `colors` 表，**写错键名会导致整份主题被拒**；
- Lua：
  - 文件第一个可执行语句之前不要有 BOM；
  - 不要用 `goto`、协程、`pcall` 包大段逻辑（可以小范围用）；
  - 每个页面函数形如 `function(t, m)`，`t` 是秒（浮点），`m` 是模型表；
  - `input(page, t, dt, m, k)` 返回 `true` = 吃掉输入；`false` = 交给原生；
  - 不要每帧创建大表（GC 在 PSP 上很敏感），常量放文件顶部；
  - 单个绘制函数不要传超过 512 字节的文本（会被拒绝并报错）。

---

## 4. 页面与模型（只读）

页面名（`pages` 的 key）：
`boot wifi home songs player lyrics search downloads cache settings about account grid square comments mine video sources skins`

常用模型字段（完整表见 API 文档，**只读**）：
`page width height frame_dt song artist song_id paused position_ms duration_ms volume liked play_mode online list_kind list_title songs cards download_count query query_len search_edit kb_row kb_col suggestions ime_mode ime_active ime_pre ime_candidates search_history search_chips chip_row chip_selected hist_selected assoc network_kbps wlan_enabled battery clock settings ...`

> 取值防御：字段可能是 `nil`（旧版程序），统一写 `(m.song or "")`、`(m.list_kind or 0)`。

---

## 5. 常用 API 速记

```lua
ui.rect(x,y,w,h,color)
ui.round(x,y,w,h,r,color)
ui.text(x,y,text,font,color)          -- font: 0=12 1=16 2=22 3=32
ui.label(x,y,w,text,font,color)       -- 超宽自动省略号
ui.icon(name,x,y,size,color)
ui.line(x1,y1,x2,y2,color,width)
ui.circle(x,y,r,color)
ui.cover(x,y,w,h)                     -- 当前歌曲封面，返回 bool
ui.image("asset_id",x,y,w,h)
ui.sound("asset_id")
ui.text_width(text,font)
ui.clip(x,y,right,bottom) / ui.unclip()
app.dispatch("action", "arg")
app.option("pref_id")
```

不确定参数顺序时，**去官方 `metro`/`xmb` 的 `skin.lua` 里搜同名调用**，照抄用法。

---

## 6. 任务提示词模板

见 [AI-PROMPT.txt](AI-PROMPT.txt)。三个最常用的变体：

- **新主题**：给出风格、参考皮肤、要覆盖的页面；
- **改现有主题**：给出目标文件与具体差异（颜色/布局/交互），要求最小改动；
- **加字体**：给出 TTF、字号、需要的文本，生成子集并改 `skin.json`。

---

## 7. 验收标准（缺一不算完成）

1. `validate_skin_runtime.py` 输出 `VALID`；
2. JSON 能 `python -c "import json..."` 解析通过；
3. `skin.lua` 在宿主机运行时不报错（校验脚本已覆盖加载与各页面首帧）；
4. 所有文本都用了安全的字段取值（`or ""`）；
5. 报告里写清楚：改了哪些文件、验证命令与输出、**未验证的部分**（如真机手感、实际音效）。

---

## 8. 常见坑（AI 最容易犯）

| 坑 | 症状 | 修法 |
|---|---|---|
| 编造 `ui.*` 函数或参数顺序 | 校验报 Lua 错误 | 只用 API 文档里的函数；照抄官方皮肤用法 |
| 用 `m.song` 不做 nil 判断 | 白屏/报错 | `(m.song or "")` |
| 颜色键写错（如 `bg`） | 主题被拒/不生效 | 只用文档里的 15 个颜色键 |
| JSON 写注释/尾逗号 | 加载失败 | 纯标准 JSON |
| 页面函数名拼错（`player` vs `Player`） | 该页回落原生 | 用文档里的页面名 |
| 每帧新建表/字符串拼接 | 卡顿 | 常量提到文件顶部，复用变量 |
| 覆盖官方皮肤却保留原 ID | 群里分发冲突 | 改 `id` 和目录名 |
| 声称"已验证真机"但没跑 | 交付不实 | 真机没测就写"未在 PSP 实机验证" |
