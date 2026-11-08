# 输入法与主题字体（实现说明）

两份新能力都走「离线生成数据 + 运行时只读」的路子，PSP 上不做动态分配。

---

## 一、输入法

### 1. 模块

| 文件 | 作用 |
|---|---|
| `src/ime.h/.c` | 状态机：拼音 / 假名、preedit、候选、上屏 |
| `src/ime_dict.h` | 词条类型 + 引入生成的数据头 |
| `src/ime_dict_data.h` | **生成文件**（勿手改），由 `tools/build_ime_dict.py` 产出 |
| `tools/ime/pinyin.txt` | 拼音词表 `词|全拼|词频` |
| `tools/ime/japanese.txt` | 日语词表 `词|平假名读音|词频` |
| `tools/build_ime_dict.py` | 生成 + 过滤（字库没有的字直接丢弃并告警） |
| `tests/test_ime_core.py` | 宿主机回归：前缀、罗马字规则、候选、退格 |

### 2. 拼音

- 键 = **无声调全拼连写**（`zhoujielun`），因此不需要切分音节；
- 候选 = 前缀匹配，按词频降序；同频按 key 排序保证稳定；
- `ime_feed('a'..'z')` 追加字母，`-` 在拼音模式无意义但保留；
- 上屏：`ime_select(i, buf, cap)`。

### 3. 日语

两层：

1. **罗马字 → 平假名**（`k_romaji[]` 规则表，最长匹配）：
   - `nn` → ん；`n` + 辅音 → ん；`n` + 元音走 な行；
   - 双写辅音 → っ（`kitte` → きって）；
   - `-` → ー；`la/xa` → ぁ；`ltu/xtu` → っ；`v` → ゔ；
   - 末尾孤立的 `n` 在提交时才收成 ん（`kana_finish()`），避免 `na` 被拆成 ん+あ。
2. **平假名 → 汉字**（`k_ime_kana[]`，键 = 整条读音）：
   - 先完全匹配（`よるにかける` → 夜に駆ける），再前缀匹配；
   - 永远把「假名原文」和「片假名」也放进候选（词库没有也能搜）；
   - 不做分词/Viterbi：搜索框要的是整条标题，词条级匹配够用。

### 4. 词表扩充

```bash
# 1) 往 tools/ime/pinyin.txt 加行：词|全拼|词频(1-9)
# 2) 重新生成（会校验字库覆盖，缺字的词会打印告警并丢弃）
python -B tools/build_ime_dict.py
# 3) 跑测试
python -B tests/test_ime_core.py
```

后续接 CC-CEDICT / JMdict 时，只需在生成脚本里多一个「读取外部源」的入口；
运行时格式不用改（有序数组，二分前缀）。

### 5. 与搜索页的连接

见 [SEARCH-UI.md](SEARCH-UI.md)：`main.c:input_search()` 把字母喂给 `ime_feed()`，
`ui.c:screen_search()` 画 preedit 与候选，皮肤从模型字段读同样的数据。

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
