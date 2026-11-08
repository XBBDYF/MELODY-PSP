# 发布前检查清单

## 结构

- [ ] 目录是 `skins/<id>/`，`<id>` 与 `skin.json` 的 `id` 完全一致
- [ ] `id` 只用 `a-z 0-9 _ -`，长度 ≤ 32
- [ ] `schema` 是 `1`
- [ ] `name`（≤48）、`author`、`version`（≤16）、`description`（≤160）都填了
- [ ] `dark` 与配色一致（深色主题写 `true`）
- [ ] 没有多余文件：`skin-prefs/`、`data.txt`、缓存、账号信息
- [ ] 引用了第三方素材时，有 `CREDITS.txt` 写明来源和许可

## JSON

- [ ] 是标准 JSON（没有注释、尾逗号、单引号）
- [ ] `python -c "import json,pathlib,sys; json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))" skins/<id>/skin.json` 通过
- [ ] 颜色只用 15 个合法键：`background panel panel2 text secondary muted line accent hot row white black success warning shadow`
- [ ] 所有颜色是 `"#RRGGBB"`
- [ ] `pages` 里的页面名都在合法列表内
- [ ] 每个页面 widget ≤ 32 个

## Lua（有脚本时）

- [ ] `return { pages = {...}, input = function(...) ... end }` 形态正确
- [ ] 所有 `m.xxx` 都有 `or ""` / `or 0` / `or {}` 兜底
- [ ] 没有 `io.*` / `os.*` / `require` / `dofile` / `loadfile`
- [ ] 没有每帧创建大表或字符串拼接
- [ ] `input` 返回 `true` 的分支都真的处理了该按键
- [ ] `ui.clip` 都配了 `ui.unclip`

## 字体（有 `fonts[]` 时）

- [ ] `size` 是 12/16/22/32
- [ ] 合计 ≤ 1.5MB，单文件 ≤ 896KB
- [ ] `validate_skin_fonts.py` 通过
- [ ] 声明了 `license` 时，`fonts/LICENSE.txt` 存在
- [ ] 只包含主题实际用到的字（避免打包过大）

## 校验命令

```bash
python -B TOOLS/validate_skin_runtime.py skins/<id>
python -B TOOLS/validate_skin_fonts.py   skins/<id>
```

- [ ] 两条命令都输出 VALID / 无错误

## 视觉与体验

- [ ] 首页、播放页、搜索页、设置页都进得去、出得来
- [ ] 长歌名/长歌手名不会溢出到屏幕外（用 `ui.label` 或截断）
- [ ] 深色/浅色两种 `dark` 下文字都看得清
- [ ] 不遮挡原生小播放器/状态栏（除非 `chrome` 里显式隐藏）
- [ ] 480×272 边界内没有元素被裁切

## 打包

- [ ] zip 内层级是 `skins/<id>/...`（解压即用）
- [ ] 文件名是 `<id>.zip`
- [ ] `preview.png` 是 480×272 PNG
- [ ] 在群里写清楚：版本、适用程序版本、已知限制
