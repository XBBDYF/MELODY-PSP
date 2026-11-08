# 参考：主题自带字体

> 用途：让主题用上楷体/宋体/日文字体，而不必把整个字库塞进程序。
> 格式与内置字库相同（PSPF v1），绘制代码零改动。

---

## 1. 什么时候需要

- 想让标题用楷体/衬线体，但内置只有黑体点阵；
- 需要内置字库没有的字（罕见字、日文汉字、特殊符号）；
- 想做「书法风」「报纸风」这类字体本身就是风格的皮肤。

> 如果只是想换颜色/排版，**不要加字体**——加字体只会让主题包变大。

---

## 2. 生成（TTF/OTF → PSPF）

```bash
# 只包含用到的字（推荐，体积最小）
python -B TOOLS/make_theme_font.py C:/Windows/Fonts/simkai.ttf --size 22 \
    --chars "MELODY开始我的音乐搜索设置下载缓存皮肤" \
    -o skins/my_theme/fonts/title.bin

# 按字符集生成（体积大，谨慎）
python -B TOOLS/make_theme_font.py font.ttf --size 16 --charset gbk -o skins/my_theme/fonts/ui.bin
```

支持 `--charset gbk|gb2312|ascii|kana`、`--charset-file`、`--text-file`（从文本文件抽取所有用到的字）。

---

## 3. 声明

```json
"fonts": [
  { "size": 22, "file": "fonts/title.bin", "name": "楷体", "license": "系统字体" },
  { "size": 16, "file": "fonts/ui.bin" }
]
```

| 规则 | 值 |
|---|---|
| 可用字号 | 12 / 16 / 22 / 32（只替换声明的字号） |
| 数量 | 最多 4 项 |
| 总预算 | ≤ 1.5MB（1536KB） |
| 单文件 | ≤ 896KB |
| 缺字 | 自动回退内置同字号 → 32→22→16 回退链 |
| license 字段 | 可选；填了就要在 `fonts/LICENSE.txt` 放许可文本 |

---

## 4. 校验

```bash
python -B TOOLS/validate_skin_fonts.py skins/my_theme
```

会检查：PSPF 头、版本、记录长度、码点升序、越界、总预算、license 文件是否存在。

---

## 5. 运行表现

- 主题应用成功后日志：`skin font: fonts/title.bin -> font22 110KB`；
- 任一字体失败 → 整体回滚到内置，日志：`skin font: rolled back (<原因>)`；
- 主题字体只在**声明过的字号**生效，其它字号仍用内置。

---

## 6. 体积经验

| 内容 | 22px 大约 |
|---|---|
| 30 个汉字 | 3–6 KB |
| 300 个汉字 | 30–50 KB |
| GB2312 全集（6763 字） | 800–1000 KB（可能超单文件上限） |
| GBK 全集（21886 字） | 超过预算，不要用于主题 |

**建议**：标题字体只放标题用到的字；正文字体不放，继续用内置。
