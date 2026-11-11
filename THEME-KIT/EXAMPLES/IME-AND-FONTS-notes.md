# 输入法与主题字体（实现说明）

核对：2026-11-11。输入法为离线生成表、二分范围查询、固定候选缓冲。

## 一、中文输入法

### 缺字根因与修复

旧生成器每音节截15字、只取多音字第一读音，运行时又限16个单字、主题模型只传前6个，导致`zhu`缺“祝”等。
现保留全部内置字体覆盖的汉字及每个已登记读音：20,892个不同汉字、27,285条字音关系、420个音节；`zhu`组174字，最大`yi`组461字。
`IME_MAX_CAND=512`，候选一屏4个但可左右移动到后面的字。真实Lua运行时测试覆盖`zhu`含祝、`yi`超过400个候选和反复绘制的堆预算。

### 中文词库与联想

- 自建完整拼音覆盖表：`tools/ime/pinyin.txt`；优先使用人工完整词读音，例音乐、重庆、祝福、生日快乐。
- jieba v0.42.1 `dict.txt`：MIT，Copyright (c) 2013 Sun Junyi。离线选择频率最高的最多8192个、2–6字、字体覆盖的词，与人工表去重，生成8,313个中文词条。
- pinyin-data：MIT，Copyright (c) 2016 mozillazg；原始读音表、许可均保留。
- 不加载jieba Python运行时，不在PSP做分词或联网下载词库。生成文件约419KB；PSP编译`ime.o`只读代码/数据约358KB，固定候选等BSS约12KB。
- 无人工覆盖的多音字词读音采用单字首读音拼接，可能有错误；词组覆盖表可继续修正。不是完整句子输入法，不声称全部中文词都收录。

候选顺序：整词完全匹配 → 同音节单字（不再截15/16字）→ 相关词首字 → 词频排名最高的前缀词（32个）。
词语排名扫描完整匹配区间，不再只看按字母排序的前64项。

`ime_assoc(text,out,max)`：中文按文本前缀索引、拼音按key二分匹配，词频排序，最多32条；在拼写、上屏和删除后刷新。
界面仍为上排联想、下排候选、右侧历史；○上屏，△跳候选/联想，键盘←删除。

### 生成与验证

```bash
python -B tools/build_ime_dict.py
python -B tests/test_ime_core.py
python -B tests/test_ime_coverage.py
python -B tests/test_skin_runtime.py
```

词频文件来源和完整MIT文本见 `tools/ime/jieba/SOURCE.md`、`tools/ime/jieba/LICENSE`；单字读音许可见`tools/ime/pinyin-data.LICENSE`。

## 日语

保留63个启动词条，罗马字→平假名状态机、整读音查汉字、平假名/片假名兜底。未做日文分词。
英文模式直接输入；□循环拼音/假名/英文。

---

## 二、主题自带字体

### 1. 格式

与内置字库相同的 **PSPF v1**：

```
u32 "PSPF" | u32 version=1 | u32 count | u32 rec_size | u32 records_offset
records（按码点升序）: u32 codepoint | u8 w | u8 h | 1bpp 位图
```

好处：`gfx.c` 的 `find_glyph()` / `draw_glyph()` 一行不用改，只是多一个查找来源。

### 2. 声明

```json
{
  "fonts": [
    { "size": 22, "file": "fonts/title.bin", "name": "Noto Serif JP", "license": "OFL-1.1" },
    { "size": 16, "file": "fonts/ui.bin" }
  ]
}
```

- `size` 只允许 12/16/22/32；
- 最多 4 项，合计 ≤ 1.5MB（`FONT_ARENA_BUDGET`），单文件 ≤ 896KB；
- 只替换声明过的字号，其余继续用内置字库；
- 主题字体缺字时自动回退内置同字号，再走 32→22→16 的回退链。

### 3. 运行时

| 文件 | 作用 |
|---|---|
| `src/skin_font.c` | 读 `skin.json` → 读字库进 arena → 校验 → 挂到 gfx；失败整体回滚 |
| `src/gfx.c` | `gfx_theme_font_attach/clear/ready`；`pick_glyph()` 先查主题字体 |
| `src/skin_runtime.c` | 主题应用成功后 `skin_font_apply(root)`；重置/切换时 `skin_font_release()` |
| `tools/make_theme_font.py` | TTF/OTF → PSPF；`--charset gbk|gb2312|ascii|kana`、`--chars`、`--text-file` |
| `tools/validate_skin_fonts.py` | 不装卡也能提前校验：头、边界、升序、预算、LICENSE |
| `tests/test_skin_font.py` | `skin_font_parse` / `skin_font_validate` 宿主机回归 |

日志：`skin font: fonts/title.bin -> font22 110KB`；
失败：`skin font: rolled back (<原因>)`，原因同时存进 `skin_font_last_error()`。

### 4. 给主题作者的最小流程

```bash
# 只要标题里出现过的字 → 体积最小
python -B tools/make_theme_font.py "C:/Windows/Fonts/simkai.ttf" --size 22 \
    --chars "MELODY开始推荐音乐我的搜索设置" -o skins/mytheme/fonts/title.bin

# 校验
python -B tools/validate_skin_fonts.py skins/mytheme

# 在 skin.json 里加 fonts 段，然后
python -B tools/validate_skin_runtime.py skins/mytheme
```

### 5. 注意

- 主题字体是**子集**，别指望它覆盖全部汉字——回退链就是为这个准备的；
- 1.5MB arena 只在有主题字体时分配，切主题即释放；
- 字体许可要放 `fonts/LICENSE.txt`（OFL 要求保留字体名与许可），并在
  `THIRD-PARTY-NOTICES.md` 登记；
- 4bpp 抗锯齿属于格式 v2，本轮未实现（当前只有 1bpp）。
