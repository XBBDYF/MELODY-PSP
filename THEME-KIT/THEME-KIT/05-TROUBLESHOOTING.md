# 常见问题（主题）

## 1. 主题不出现在「皮肤」列表里

- 目录层级错：必须是 `skins/<id>/skin.json`，不能多套一层（如 `skins/mytheme/mytheme/skin.json`）；
- `skin.json` 解析失败：JSON 不能有注释、尾逗号、单引号；
- `id` 与目录名不一致，或 `id` 含大写/中文/空格；
- 文件是 UTF-8 **带 BOM**：部分解析器会失败，保存为「UTF-8 无 BOM」。

## 2. 选了主题，界面没变化

- 只写了 `script: "skin.lua"` 但文件不存在 → 回落原生；
- `pages` 里页面名拼错（比如 `Player`）→ 该页回落原生；
- 纯配色主题改完颜色需要**重启程序**（配色在启动时读）；
- 官方皮肤名与目录里的 `id` 冲突时，以目录为准——确认你选的是自己的 ID。

## 3. 白屏 / 黑屏 / 一进页面就退出

- `m` 字段没兜底：`m.song` 为 nil 时调 `string.sub`/`#` 会报错；
- 每帧创建巨大表或字符串拼接导致内存不足；
- 在 Lua 里调了不存在的 `ui.*` / `app.*`（拼写错误）；
- `ui.clip` 之后忘了 `ui.unclip()`，后续绘制全被裁掉。

排查：用校验脚本跑一遍，会给出 Lua 错误行号：
```bash
python -B TOOLS/validate_skin_runtime.py skins/my_theme
```

## 4. 文字显示成方块 / 问号

- 内置字库是 GBK 点阵：生僻字、部分日文汉字、`‹ ›`、`①` 这类符号可能没有；
- 解决：用主题字体补字（[04-REFERENCE-FONTS.md](04-REFERENCE-FONTS.md)）；
- 临时办法：换 ASCII 等价符号（`<` `>` `-` `...`）。

## 5. 主题字体不生效

- `size` 不是 12/16/22/32；
- 字体文件路径相对主题目录写错；
- 文件超过 896KB 或总预算 1.5MB；
- PSPF 头不合法（用 `validate_skin_fonts.py` 看具体原因）；
- 声明的字号和你实际绘制的字号不一致（比如声明 22，却用 font=1/16px 画）。

## 6. 输入没反应

- `input` 返回了 `true` 却没有处理按键 → 原生也收不到；
- 判断页面名写错，`input` 里永远进不去；
- 只用 `held` 没处理 `pressed`，导致单击无效；
- 在 `input` 里调用了绘制函数（会打乱帧）。

## 7. 卡顿 / 掉帧

- 每帧新建表、拼字符串、`string.format`；
- 每帧调用 `ui.cover` 多次（封面解码有成本）；
- 大面积 `ui.gradient` / 逐个像素画背景；
- 在 `pages` 函数里做排序/遍历大列表。

修法：常量提取到文件顶部；封面只画一次；用 `ui.rect` 代替渐变；列表只画可见行。

## 8. 音效不响

- `assets` 里没有声明该 `id`，或 `type` 不是 `sound`；
- WAV 不是 16-bit PCM / 采样率过高；
- `ui.sound` 在 `capture_page`/过渡阶段被调用（会返回 false）。

## 9. 主题选项不显示 / 读不到

- `preferences` 的 `id` 与 `app.option("...")` 不一致；
- `type` 只能是 `choice` / `toggle`；
- `default` 越界（choice 超出 `options` 数量）。

## 10. 群里别人装了我的主题看不到预览图

- `preview.png` 必须是 480×272 的 PNG，放在主题根目录；
- zip 里层级要保留 `skins/<id>/...`，不要只压缩 `skin.json`。

## 11. 校验脚本报 `lua error` 但 PSP 上能跑

- 宿主机是 64 位、PSP 是 32 位整数：大数位移/取模结果会不同；
- 校验脚本比 PSP 更严格（如文本长度、坐标范围），以校验为准更安全。

## 12. 想看运行时日志

主题应用的日志在程序目录的 `log.txt`：
```text
skin applied id=metro version=5.7 script=1
skin font: fonts/title.bin -> font22 110KB
skin font: rolled back (bad magic)
```
看到 `rolled back` 就是字体问题；`skin applied` 后面没有 `script=1` 说明只加载了配色。
