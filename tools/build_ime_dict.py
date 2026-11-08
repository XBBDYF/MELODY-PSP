"""Build the built-in IME dictionaries from tools/ime/*.txt + pinyin-data.

Outputs src/ime_dict_data.h:
  k_ime_pinyin[]   词条：key=全拼连写, word=中文, freq
  k_ime_kana[]     日语：key=平假名读音, word=写法, freq
  k_ime_chars[]    单字：key=音节(无声调), chars=该音节的常用字（按 GB2312 常用序）

pinyin-data (Unicode → 读音) 由 tools/ime/pinyin-data.txt 提供（mozillazg/pinyin-data）。
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "tools" / "ime"
OUT = ROOT / "src" / "ime_dict_data.h"
FONT = ROOT / "src" / "font16.bin"
MAX_CHARS_PER_SYL = 15

# 常用字频率序（约 500 字，用来给同一音节的单字排序；不在表里的排在后面按 GB 码位）
FREQ_ORDER = (
    "的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情者最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指几九区强放决西被干做必战先回则任取据处府七队南给色光门即保治北造百规热领七海口东导器压志世金增争济阶油思术极交受联什认六共权收证改清己美再采转更单风切打白教速花带安场身车例真务具万每目至达走积示议声报斗完类八离华名确才科张信马节话米整空元况今集温传土许步群广石记需段研界拉林律叫且究观越织装影算低持音众书布复容儿须际商非验连断深难近矿千周委素技备半办青省列习响约支般史感劳便团往酸历市克何除消构府称太准精值号率族维划选标写存候毛亲快效斯院查江型眼王按格养易置派层片始却专状育厂京识适属圆包火住调满县局照参红细引听该铁价严龙飞"
)

# 补充：主频表之后的常见字/姓名/歌名用字，保证同音节里排在 GB 码位之前
EXTRA_BOOST = '陈晨辰沉忱诚城承呈程乘成称臣郴尘张章赵钱孙李周吴郑王冯褚卫蒋沈韩杨朱秦尤许何吕施林俊杰紫棋奕迅荣浩之谦晨宇不易苏泷嵩菲英学友德华五月天周杰伦邓紫棋陈奕迅薛之谦李荣浩毛不易华晨宇汪苏泷许嵩王菲那英稻香晴天七里香青花瓷夜曲屋顶花海泡沫演员丑八怪后来童话十年中国音乐歌手歌曲专辑搜索播放下载歌词评论喜欢推荐排行每日我的设置关于本地缓存主题皮肤音量暂停继续上一首下一首循环随机单曲'
FREQ_ORDER = FREQ_ORDER + EXTRA_BOOST


# 带调 → 无调
TONE = {
    "ā": "a", "á": "a", "ǎ": "a", "à": "a",
    "ē": "e", "é": "e", "ě": "e", "è": "e",
    "ī": "i", "í": "i", "ǐ": "i", "ì": "i",
    "ō": "o", "ó": "o", "ǒ": "o", "ò": "o",
    "ū": "u", "ú": "u", "ǔ": "u", "ù": "u",
    "ǖ": "v", "ǘ": "v", "ǚ": "v", "ǜ": "v", "ü": "v",
    "ń": "n", "ň": "n", "ǹ": "n", "ḿ": "m", "m̀": "m",
}


def strip_tone(s):
    out = []
    for ch in s:
        out.append(TONE.get(ch, ch))
    return "".join(out).lower()


def load_font_codepoints(path):
    import struct
    b = path.read_bytes()
    magic, ver, count, rec, off = struct.unpack_from("<5I", b, 0)
    assert magic == 0x46505350
    return {struct.unpack_from("<I", b, off + i * rec)[0] for i in range(count)}


def gb_order(ch):
    """GB2312 一级字在前，二级在后；同级按 GB 码位（近似常用序）。"""
    try:
        b = ch.encode("gb2312")
    except UnicodeEncodeError:
        return (2, 0x10000)
    code = (b[0] << 8) | b[1]
    level = 0 if 0xB0A1 <= code <= 0xD7F9 else 1
    return (level, code)


def parse_pinyin_data(path, font_cps):
    groups = {}
    pat = re.compile(r"^U\+([0-9A-Fa-f]+):\s*([^#]+)")
    for line in path.read_text(encoding="utf-8").splitlines():
        m = pat.match(line)
        if not m:
            continue
        cp = int(m.group(1), 16)
        if cp not in font_cps:
            continue
        first = m.group(2).split(",")[0].strip()
        syl = strip_tone(first)
        if not syl or not re.fullmatch(r"[a-z]+", syl):
            continue
        ch = chr(cp)
        groups.setdefault(syl, []).append(ch)
    rank = {}
    for i, ch in enumerate(FREQ_ORDER):
        rank.setdefault(ch, i)      # 重复字保留第一次出现（更靠前）

    def sort_key(ch):
        r = rank.get(ch)
        return (0, r) if r is not None else (1, gb_order(ch))

    for syl in groups:
        groups[syl].sort(key=sort_key)
        groups[syl] = groups[syl][:MAX_CHARS_PER_SYL]
    return groups


def parse(path, key_lower):
    rows = []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("|")
        if len(parts) != 3:
            sys.exit(f"{path.name}:{n}: expected word|key|freq")
        word, key, freq = parts[0].strip(), parts[1].strip(), parts[2].strip()
        if key_lower:
            key = key.lower()
        if not word or not key:
            sys.exit(f"{path.name}:{n}: empty word/key")
        if not freq.isdigit() or not 1 <= int(freq) <= 9:
            sys.exit(f"{path.name}:{n}: freq must be 1..9")
        if any(ch.isspace() for ch in key):
            sys.exit(f"{path.name}:{n}: key must not contain spaces")
        rows.append((key, word, int(freq)))
    rows.sort(key=lambda r: (r[0], -r[2], r[1]))
    return rows


def emit_entries(rows, name, out):
    out.append(f"static const ImeEntry {name}[] = {{\n")
    for key, word, freq in rows:
        out.append('    {"%s", "%s", %d},\n' % (key, word, freq))
    out.append("};\n")
    out.append(f"static const int {name}_count = (int)(sizeof({name}) / sizeof({name}[0]));\n\n")


def main():
    js = (SRC / "pinyin-data.txt")
    if not js.exists():
        sys.exit("缺 tools/ime/pinyin-data.txt（mozillazg/pinyin-data）")
    font_cps = load_font_codepoints(FONT)
    groups = parse_pinyin_data(js, font_cps)

    py = parse(SRC / "pinyin.txt", True)
    ja = parse(SRC / "japanese.txt", False)

    kept_py = [r for r in py if all(ord(c) < 0x80 or ord(c) in font_cps for c in r[1])]
    kept_ja = [r for r in ja if all(ord(c) < 0x80 or ord(c) in font_cps for c in r[1])]

    body = ["/* Generated by tools/build_ime_dict.py — do not edit by hand. */\n",
            "/* Sources: tools/ime/pinyin.txt, tools/ime/japanese.txt, tools/ime/pinyin-data.txt */\n\n",
            "#ifndef MELODY_IME_DICT_DATA_H\n#define MELODY_IME_DICT_DATA_H\n\n",
            '#include "ime_dict.h"\n\n']

    emit_entries(kept_py, "k_ime_pinyin", body)
    emit_entries(kept_ja, "k_ime_kana", body)

    # 单字表：按音节排序，供二分查找
    body.append("static const ImeCharGroup k_ime_chars[] = {\n")
    for syl in sorted(groups):
        chars = "".join(groups[syl])
        body.append('    {"%s", "%s"},\n' % (syl, chars))
    body.append("};\n")
    body.append("static const int k_ime_chars_count = (int)(sizeof(k_ime_chars) / sizeof(k_ime_chars[0]));\n\n")
    body.append("#endif\n")

    OUT.write_text("".join(body), encoding="utf-8")
    print(f"pinyin words: {len(kept_py)}, japanese: {len(kept_ja)}, "
          f"char syllables: {len(groups)}, chars: {sum(len(v) for v in groups.values())}")
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
