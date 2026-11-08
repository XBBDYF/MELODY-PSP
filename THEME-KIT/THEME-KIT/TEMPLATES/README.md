# 模板说明

| 目录 | 内容 | 适合 |
|---|---|---|
| `minimal/` | `skin.json` + `skin.lua`（首页/播放页/搜索页三页示例） | 想写脚本主题的人 |
| `widgets/` | 只有 `skin.json`，用 `pages.home.widgets` 摆块 | 不想写代码的人 |

## 用 minimal 起手

1. 整目录复制到 `skins/my_theme/`；
2. 改 `skin.json`：`id`（必须等于目录名）、`name`、`author`、`version`；
3. 按需删掉不想要的页面函数（删掉即回落原生）；
4. 校验：
   ```bash
   python -B TOOLS/check_skin.py skins/my_theme
   ```
5. 想加图片/音效时，在 `skin.json` 加 `assets`，文件放 `assets/` 目录。

## 用 widgets 起手

1. 复制 `widgets/skin.json` 到 `skins/my_widget/skin.json`；
2. 改 `id` 与颜色；
3. 坐标范围 0..960，超出会被截断；
4. 想加文字就加一条 `type: "text"`；想绑状态就用 `bind`（字段名见 API 文档 §6）。

> 两个模板都只覆盖了部分页面，其余页面自动使用原生界面——这是推荐做法，省事且不会漏功能。
