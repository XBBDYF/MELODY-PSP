> 确认键适配更新：见[13-CONFIRM-BUTTONS.md](13-CONFIRM-BUTTONS.md)，默认跟随系统，○/×提示只转换一次。

> 2026-11-11后续：省电皮肤`power_profile`将锁定全局性能选项；ECO支持配色，极致模式本地/缓存与歌词/封面开关。见[09-POWER-PROFILES.md](09-POWER-PROFILES.md)。独立词库见[10-IME-PACK.md](10-IME-PACK.md)。

# MELODY 主题制作包（THEME-KIT）

> 面向两类人：**人类主题作者** 和 **AI 助手**。
> 目标：不看整个程序源码，也能做出一套能装进 PSP、不会崩、不会显示方块的主题。

- 适用版本：MELODY 4.1.1+（`schema: 1`，Lua 5.4，画面 480×272）
- 主题放置：`ms0:/PSP/GAME/MELODY/skins/<主题ID>/`
- 一份主题 = 一个文件夹（`skin.json` + 可选 `skin.lua` + 可选 `assets/` + 可选 `preview.png`）

---

## 电源控制更新（2026-11-11）

ECO示例已接入程序省电、30/60/120/300秒背光超时、222/266/333MHz时钟设置；需要本轮开发构建，旧4.1.1未必提供这些能力。见 [08-POWER-CONTROLS.md](08-POWER-CONTROLS.md)。实际续航和固件背光表现须真机验证。

## 1. 三条路，选一条

| 你想做的事 | 需要什么 | 看哪篇 |
|---|---|---|
| 只换配色 / 背景 / 强调色 | **一个 `skin.json`**，不用 Lua | [01-HUMAN-GUIDE.md](01-HUMAN-GUIDE.md) §3 |
| 自定义页面排版（磁贴、横排、纸感…） | `skin.json` + `skin.lua` | [01-HUMAN-GUIDE.md](01-HUMAN-GUIDE.md) §4 |
| 让 AI 帮你做/改主题 | 把 [AI-PROMPT.txt](AI-PROMPT.txt) 贴给 AI，并让它读 [02-AI-GUIDE.md](02-AI-GUIDE.md) | [02-AI-GUIDE.md](02-AI-GUIDE.md) |
| 想做"省电/轻量"主题 | 少填充、不逐帧动画、不带图；**深色不等于省电**（LCD 背光常亮） | [01-HUMAN-GUIDE.md](01-HUMAN-GUIDE.md) §12 |

---

## 2. 5 分钟做出第一套配色主题

```text
skins/
  my_night/
    skin.json
    preview.png      ← 可选，皮肤列表里的预览图（480×272）
```

`skins/my_night/skin.json`：

```json
{
  "schema": 1,
  "id": "my_night",
  "name": "夜色",
  "author": "你的名字",
  "version": "1.0",
  "description": "深蓝底 + 亮蓝强调色",
  "dark": true,
  "colors": {
    "background": "#101827",
    "panel": "#182337",
    "panel2": "#22334B",
    "text": "#F5F7FF",
    "secondary": "#B4BFD4",
    "muted": "#8591A7",
    "line": "#344761",
    "accent": "#5599EE",
    "hot": "#75B8FF"
  }
}
```

拷到卡上 → 进「皮肤」页 → 选中 → 立即生效。
没写的颜色会继承深色/浅色默认板（由 `dark` 决定）。

---

## 3. 目录说明

| 文件 | 作用 |
|---|---|
| [01-HUMAN-GUIDE.md](01-HUMAN-GUIDE.md) | 人类教程：从配色到脚本页面、资源、字体、发布 |
| [02-AI-GUIDE.md](02-AI-GUIDE.md) | AI 工作规则：边界、流程、硬约束、验收 |
| [AI-PROMPT.txt](AI-PROMPT.txt) | 可直接粘贴给 AI 的提示词（含约束与命令） |
| [03-REFERENCE-API.md](03-REFERENCE-API.md) | `skin.json` 全字段、`ui.*` / `app.*`、模型字段、页面名、颜色名 |
| [04-REFERENCE-FONTS.md](04-REFERENCE-FONTS.md) | 主题自带字体（PSPF、生成、预算、回退） |
| [05-TROUBLESHOOTING.md](05-TROUBLESHOOTING.md) | 常见错误：白屏、方块字、不显示、卡顿、校验失败 |
| [06-CHECKLIST.md](06-CHECKLIST.md) | 发布前逐项检查 |
| [07-EXAMPLES.md](07-EXAMPLES.md) | 9 套官方皮肤分别该怎么读、抄哪一段 |
| [TEMPLATES/](TEMPLATES/) | 空主题骨架（配色版 / 脚本版 / widget 版） |
| [EXAMPLES/](EXAMPLES/) | 可直接运行的示例：paper（脚本版）、ocean（配色版）、**eco（省电原型）** + 搜索页说明 |
| [TOOLS/](TOOLS/) | 三个离线校验/生成脚本（Python 3，无需编译） |

---

## 4. 校验（强烈建议，PSP 上改错很难看出来）

```bash
# 主题结构和 Lua 运行（构建一次宿主机运行时，之后秒验）
python -B TOOLS/validate_skin_runtime.py skins/my_night

# 主题字体（有 fonts[] 时才需要）
python -B TOOLS/validate_skin_fonts.py skins/my_night

# 字体生成：TTF/OTF → PSPF 子集
python -B TOOLS/make_theme_font.py myfont.ttf --size 22 --chars "常用字" -o skins/my_night/fonts/title.bin
```

三个脚本都是**只读校验**，不会改你的主题文件。

---

## 5. 分发（QQ 群 1103558445）

- 安装包、主题包只走群文件，不挂 GitHub Releases；
- 群里建议发 **zip**：`<主题ID>.zip`，解压后是 `skins/<主题ID>/` 目录；
- 主题里**不要**带 `skin-prefs`、账号、缓存、`data.txt` 等个人数据；
- 引用别人的字体/图片/音效时，在主题目录放 `CREDITS.txt` 写明来源与许可。

---

## 6. 版本与兼容

- `schema` 目前只有 `1`；
- `min_app_version` 填你验证过的最低版本（例如 `"4.1.1"`），旧版程序看到更高版本会提示但仍可尝试加载；
- 新增能力（`fonts`、`preferences`、`optional_capabilities`）在旧程序上是**忽略**而不是报错，所以可以安全带上。

ECO配色已同步Lua页、原生回退和状态栏。MV启动时回收首页图片缓存，新增主题不要把大像素缓冲常驻不可释放区。
