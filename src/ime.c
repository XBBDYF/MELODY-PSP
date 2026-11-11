/*
 * MELODY PSP — 输入法核心（拼音 / 日语假名）
 *
 * 设计要点见 docs/IME.md：
 *   - 拼音词条按全拼前缀匹配；单字按最长合法首音节逐段提交；
 *   - 日语先做罗马字→平假名（确定性），再用读音在词库里查汉字候选；
 *     不做分词/Viterbi，词条级匹配够用于搜索框；
 *   - 候选保存紧凑索引；文本仅按可见窗口读取，详见 docs/IME-PACK.md。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "ime_pack.h"
#ifdef __PSP__
#include <pspiofilemgr.h>
#endif
#include "ime.h"
#include "ime_dict.h"

#define CAND_MAX IME_MAX_CAND

static int g_mode = IME_PINYIN;
static char g_pre[IME_PRE_MAX];       /* 拼音：字母；假名：罗马字缓冲 */
static char g_kana[IME_PRE_MAX];      /* 假名：已转换的平假名（UTF-8） */
typedef struct { uint32_t id, score; } CandidateRef;
static CandidateRef *g_refs;
static int g_ref_cap, g_ref_failed;
static char g_resume_path[512];
static int g_suspended;
static char g_assoc_key[IME_PRE_MAX];
static char g_assoc_words[32][IME_WORD_MAX];
static int g_assoc_valid,g_assoc_max,g_assoc_n;
static unsigned char g_seen_chars[8192];
static char g_cand[8][IME_WORD_MAX];
static char g_kata[IME_PRE_MAX];
static ImePack *g_pack;
static int  g_cand_n;
static char g_last[IME_PRE_MAX];

/* ------------------------------------------------------------------ */
/* 罗马字 → 平假名                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *romaji;
    const char *kana;
} RomajiRule;

static const RomajiRule k_romaji[] = {
    /* 拗音（长的放前面，按最长匹配） */
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
    {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"}, {"shi", "し"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"}, {"chi", "ち"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"ja", "じゃ"}, {"ju", "じゅ"}, {"jo", "じょ"}, {"ji", "じ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"dya", "ぢゃ"}, {"dyu", "ぢゅ"}, {"dyo", "ぢょ"},
    {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"},
    {"fa", "ふぁ"}, {"fi", "ふぃ"}, {"fu", "ふ"}, {"fe", "ふぇ"}, {"fo", "ふぉ"},
    {"va", "ゔぁ"}, {"vi", "ゔぃ"}, {"vu", "ゔ"}, {"ve", "ゔぇ"}, {"vo", "ゔぉ"},
    {"tsu", "つ"}, {"tu", "つ"},
    {"dzu", "づ"}, {"du", "づ"},
    /* 小假名 */
    {"la", "ぁ"}, {"li", "ぃ"}, {"lu", "ぅ"}, {"le", "ぇ"}, {"lo", "ぉ"},
    {"xa", "ぁ"}, {"xi", "ぃ"}, {"xu", "ぅ"}, {"xe", "ぇ"}, {"xo", "ぉ"},
    {"ltu", "っ"}, {"xtu", "っ"}, {"ltsu", "っ"}, {"xtsu", "っ"},
    {"lya", "ゃ"}, {"lyu", "ゅ"}, {"lyo", "ょ"},
    {"xya", "ゃ"}, {"xyu", "ゅ"}, {"xyo", "ょ"},
    {"lwa", "ゎ"}, {"xwa", "ゎ"},
    /* 五十音 */
    {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"sa", "さ"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"ta", "た"}, {"te", "て"}, {"to", "と"},
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha", "は"}, {"hi", "ひ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"},
    {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},
    {"wa", "わ"}, {"wo", "を"}, {"nn", "ん"}, {"n", "ん"},
    {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},
    {"za", "ざ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"da", "だ"}, {"de", "で"}, {"do", "ど"},
    {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},
    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"},
    {0, 0}
};

static int is_vowel(int ch)
{
    return ch == 'a' || ch == 'i' || ch == 'u' || ch == 'e' || ch == 'o';
}

/* 从 g_pre 开头尽量吃掉一段罗马字，转成假名追加到 g_kana。
 * 返回 1 表示这一轮有进展（需要再扫一次）。 */
static int kana_consume_one(void)
{
    int len = (int)strlen(g_pre);

    if (len <= 0) return 0;

    /* 长音符 */
    if (g_pre[0] == '-') {
        strncat(g_kana, "ー", sizeof(g_kana) - strlen(g_kana) - 1);
        memmove(g_pre, g_pre + 1, (size_t)len);
        return 1;
    }

    /* 促音：双写辅音（kk/tt/pp/ss...） */
    if (len >= 2 && g_pre[0] == g_pre[1] && !is_vowel(g_pre[0]) && g_pre[0] != 'n') {
        strncat(g_kana, "っ", sizeof(g_kana) - strlen(g_kana) - 1);
        memmove(g_pre, g_pre + 1, (size_t)len);
        return 1;
    }

    /* 拨音：n 后面跟辅音（不是元音、也不是 y）时先出 ん */
    if (g_pre[0] == 'n' && len >= 2 && !is_vowel(g_pre[1]) && g_pre[1] != 'y' && g_pre[1] != 'n') {
        strncat(g_kana, "ん", sizeof(g_kana) - strlen(g_kana) - 1);
        memmove(g_pre, g_pre + 1, (size_t)len);
        return 1;
    }

    /* 最长匹配规则表（表里 "n"/"nn" 放在最后，避免抢跑） */
    for (int i = 0; k_romaji[i].romaji; ++i) {
        int rl = (int)strlen(k_romaji[i].romaji);

        if (rl > len || strncmp(g_pre, k_romaji[i].romaji, (size_t)rl)) continue;
        /* 单字母 "n" 不即时转换：等下一个字符决定 な行 还是 ん */
        if (!strcmp(k_romaji[i].romaji, "n")) continue;
        strncat(g_kana, k_romaji[i].kana, sizeof(g_kana) - strlen(g_kana) - 1);
        memmove(g_pre, g_pre + rl, (size_t)(len - rl + 1));
        return 1;
    }
    return 0;
}

static void kana_flush_romaji(void)
{
    while (kana_consume_one()) {
    }
    /* 剩下的无法转换：原样留在 g_pre 里，作为英文/半角候选 */
}

/* 平假名 → 片假名（U+3041..U+3096 → +0x60；ゔ 同样适用） */
static void hira_to_kata(const char *src, char *out, int cap)
{
    const unsigned char *p = (const unsigned char *)src;
    int n = 0;

    while (*p && n < cap - 4) {
        unsigned int c;

        if (*p < 0x80) {
            out[n++] = (char)*p++;
            continue;
        }
        if ((*p & 0xE0) == 0xC0 && p[1]) {
            c = ((unsigned)(*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            c = ((unsigned)(*p & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            out[n++] = (char)*p++;
            continue;
        }
        if (c >= 0x3041 && c <= 0x3096 && c != 0x3094) c += 0x60;
        else if (c == 0x3094) c = 0x30F4;

        if (c < 0x800) {
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
}

/* ------------------------------------------------------------------ */
/* 候选                                                                */
/* ------------------------------------------------------------------ */


static void cand_clear(void) { g_cand_n=0; }
static int dictionary_count(int kind) {
 if(g_pack) return (int)ime_pack_count(g_pack,kind);
 return kind==0?k_ime_pinyin_count:kind==1?k_ime_kana_count:0;
}
static int dictionary_read(int kind,int i,ImePackEntry *e) {
 if(g_pack) return !ime_pack_read(g_pack,kind,(unsigned)i,e);
 if(kind==2) {
  int group=i>>16,offset=i&65535;
  if(group<0||group>=k_ime_chars_count||offset>=(int)strlen(k_ime_chars[group].chars)) return 0;
  const char *w=k_ime_chars[group].chars+offset;
  int n=((unsigned char)*w<128)?1:((unsigned char)*w<224)?2:((unsigned char)*w<240)?3:4;
  snprintf(e->key,sizeof(e->key),"%s",k_ime_chars[group].key);
  memcpy(e->word,w,(size_t)n);e->word[n]=0;e->freq=65535u-(unsigned)offset;e->kind=2;return 1;
 }
 if(kind<0||kind>1||i<0||i>=dictionary_count(kind)) return 0;
 const ImeEntry *r=kind==0?&k_ime_pinyin[i]:&k_ime_kana[i];
 snprintf(e->key,sizeof(e->key),"%s",r->key);snprintf(e->word,sizeof(e->word),"%s",r->word);e->freq=r->freq;e->kind=(unsigned)kind;return 1;
}
static void dictionary_range(int kind,const char *key,int *lo,int *hi) {
 int a=0,b=dictionary_count(kind),kl=(int)strlen(key);ImePackEntry e;
 while(a<b) { int m=a+(b-a)/2; if(!dictionary_read(kind,m,&e)) { *lo=*hi=0;return; } if(strncmp(e.key,key,(size_t)kl)<0)a=m+1;else b=m; }
 *lo=a;b=dictionary_count(kind);
 while(a<b) { int m=a+(b-a)/2;if(!dictionary_read(kind,m,&e)){*lo=*hi=0;return;}if(strncmp(e.key,key,(size_t)kl)<=0)a=m+1;else b=m; }
 *hi=a;
}
static void ref_add(int kind,unsigned int id,uint32_t score) {
 if(g_ref_failed)return;
 if(g_cand_n==g_ref_cap) {
  int cap=g_ref_cap?g_ref_cap*2:256;
  CandidateRef *next=(CandidateRef*)realloc(g_refs,(size_t)cap*sizeof(*next));
  if(!next){g_ref_failed=1;return;}
  g_refs=next;g_ref_cap=cap;
 }
 if(g_cand_n<g_ref_cap) { g_refs[g_cand_n].id=((unsigned)kind<<30)|id;g_refs[g_cand_n++].score=score; }
}
static int ref_compare(const void *a,const void *b) {
 const CandidateRef *x=(const CandidateRef*)a,*y=(const CandidateRef*)b;
 if(x->score!=y->score)return x->score>y->score?-1:1;
 return x->id<y->id?-1:x->id>y->id;
}
static void collect(int kind,const char *key) {
 int lo,hi;ImePackEntry e;dictionary_range(kind,key,&lo,&hi);
 for(int i=lo;i<hi;i++) if(dictionary_read(kind,i,&e)) {
  uint32_t stage=!strcmp(e.key,key)?0x30000000u:0x10000000u;
  ref_add(kind,(unsigned)i,stage+(e.freq>0x0fffffffu?0x0fffffffu:e.freq));
 }
}
static void char_add(unsigned int id,const ImePackEntry *e) {
 const unsigned char *p=(const unsigned char*)e->word;unsigned cp=*p;
 if(*p>=240)cp=((p[0]&7u)<<18)|((p[1]&63u)<<12)|((p[2]&63u)<<6)|(p[3]&63u);
 else if(*p>=224)cp=((p[0]&15u)<<12)|((p[1]&63u)<<6)|(p[2]&63u);
 else if(*p>=192)cp=((p[0]&31u)<<6)|(p[1]&63u);
 if(cp<65536) { unsigned char bit=(unsigned char)(1u<<(cp&7));if(g_seen_chars[cp>>3]&bit)return;g_seen_chars[cp>>3]|=bit; }
 ref_add(2,id,0x20000000u+(e->freq>0x0fffffffu?0x0fffffffu:e->freq));
}
static void char_candidates(const char *prefix) {
 int kl=(int)strlen(prefix),exact=0;ImePackEntry e;
 memset(g_seen_chars,0,sizeof(g_seen_chars));
 if(g_pack) {
  int lo,hi;dictionary_range(2,prefix,&lo,&hi);
  for(int i=lo;i<hi;i++) if(dictionary_read(2,i,&e)&&!strcmp(e.key,prefix)){exact=1;break;}
  for(int i=lo;i<hi;i++) if(dictionary_read(2,i,&e)&&(!exact||!strcmp(e.key,prefix)))char_add((unsigned)i,&e);
 } else {
  for(int i=0;i<k_ime_chars_count;i++)if(!strcmp(k_ime_chars[i].key,prefix)){exact=1;break;}
  for(int i=0;i<k_ime_chars_count;i++) {
   if(strncmp(k_ime_chars[i].key,prefix,(size_t)kl)||(exact&&strcmp(k_ime_chars[i].key,prefix)))continue;
   const char *start=k_ime_chars[i].chars,*p=start;
   while(*p) { unsigned id=((unsigned)i<<16)|(unsigned)(p-start);if(!dictionary_read(2,(int)id,&e))break;char_add(id,&e);p+=strlen(e.word); }
  }
 }
}
/* The character table is the authoritative set of legal syllables.  Prefer the
 * longest complete syllable; incomplete initials still use prefix expansion. */
static void first_syllable(const char *pre,char out[IME_PRE_MAX]) {
 size_t len=strlen(pre);if(len>6)len=6;
 for(;len;len--) {
  memcpy(out,pre,len);out[len]=0;
  if(g_pack) {
   int lo,hi;ImePackEntry e;dictionary_range(2,out,&lo,&hi);
   if(lo<hi&&dictionary_read(2,lo,&e)&&!strcmp(e.key,out))return;
  } else for(int i=0;i<k_ime_chars_count;i++)if(!strcmp(k_ime_chars[i].key,out))return;
 }
 snprintf(out,IME_PRE_MAX,"%s",pre);
}
static void build_refs(void) {
 if(g_mode==IME_PINYIN) { if(!g_pre[0])return;char syllable[IME_PRE_MAX];first_syllable(g_pre,syllable);collect(0,g_pre);char_candidates(syllable); }
 else if(g_mode==IME_KANA) {
  if(g_kana[0]) { collect(1,g_kana);ref_add(3,0,3);ref_add(3,1,2); }
  if(g_pre[0])ref_add(3,2,1);
 }
}
static void rebuild_candidates(void);

static void kana_finish(void)
{
    if (g_mode != IME_KANA) return;
    if (!strcmp(g_pre, "n")) {
        strncat(g_kana, "ん", sizeof(g_kana) - strlen(g_kana) - 1);
        g_pre[0] = 0;
        rebuild_candidates();
    }
}

static void rebuild_candidates(void)
{
 cand_clear();g_ref_failed=0;build_refs();
 if(g_ref_failed)cand_clear();
 if(g_cand_n>1)qsort(g_refs,(size_t)g_cand_n,sizeof(*g_refs),ref_compare);
 hira_to_kata(g_kana,g_kata,sizeof(g_kata));
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void ime_reset(void)
{
    g_pre[0] = 0;
    g_kana[0] = 0;
    cand_clear();
}

int ime_mode(void)
{
    return g_mode;
}

void ime_set_mode(int mode)
{
    if (mode != IME_OFF && mode != IME_PINYIN && mode != IME_KANA && mode != IME_ASCII) return;
    ime_reset();
    g_mode = mode;
}

void ime_toggle_mode(void)
{
    /* 循环：拼音 → 假名 → 英文 → 拼音 */
    if (g_mode == IME_PINYIN) ime_set_mode(IME_KANA);
    else if (g_mode == IME_KANA) ime_set_mode(IME_ASCII);
    else ime_set_mode(IME_PINYIN);
}

int ime_active(void)
{
    return g_pre[0] || g_kana[0];
}

const char *ime_preedit(void)
{
    return g_mode == IME_KANA ? g_kana : g_pre;
}

const char *ime_last_commit(void)
{
    return g_last;
}

int ime_candidate_count(void)
{
    return g_cand_n;
}

int ime_candidate_total(void) { return g_cand_n; }
const char *ime_candidate(int i)
{
 ImePackEntry e;unsigned id,kind;char *out;
 if(i<0||i>=g_cand_n)return "";
 id=g_refs[i].id&0x3fffffffu;kind=g_refs[i].id>>30;
 if(kind==3)return id==0?g_kana:id==1?g_kata:g_pre;
 out=g_cand[i%8];
 if(!dictionary_read((int)kind,(int)id,&e)) { out[0]=0;return out; }
 snprintf(out,IME_WORD_MAX,"%s",e.word);return out;
}
int ime_candidate_window(int first,char out[][IME_WORD_MAX],int max)
{
 int n=0;if(!out||first<0||max<=0)return 0;
 while(n<max&&first<g_cand_n){snprintf(out[n++],IME_WORD_MAX,"%s",ime_candidate(first++));}return n;
}
void ime_activate_pack(ImePack *prepared)
{
 ImePack *old=g_pack;if(prepared&&prepared==old)return;
 ime_reset();g_suspended=0;g_assoc_valid=0;free(g_refs);g_refs=NULL;g_ref_cap=0;g_pack=prepared;ime_pack_close(old);
 snprintf(g_resume_path,sizeof(g_resume_path),"%s",ime_pack_path(prepared));
}
/* Read only a short basename, never a path supplied by a pointer file. */
static int active_basename(const char *path,char name[81])
{
 unsigned char bytes[84];int n;
#ifdef __PSP__
 SceUID fd=sceIoOpen(path,PSP_O_RDONLY,0);if(fd<0)return 0;
 n=sceIoRead(fd,bytes,sizeof(bytes));sceIoClose(fd);
#else
 FILE *f=fopen(path,"rb");if(!f)return 0;
 n=(int)fread(bytes,1,sizeof(bytes),f);if(ferror(f))n=-1;fclose(f);
#endif
 if(n<=0||n>82)return 0;
 if(bytes[n-1]=='\n'){n--;if(n>0&&bytes[n-1]=='\r')n--;}
 if(n<=0||n>80)return 0;
 for(int i=0;i<n;i++) {
  unsigned char c=bytes[i];
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return 0;
 }
 memcpy(name,bytes,(size_t)n);name[n]=0;
 return strcmp(name,".")&&strcmp(name,"..");
}
int ime_load_dictionary(const char *app_dir)
{
 char directory[400],path[512],name[81];ImePack *p=NULL;int r;
 if(!app_dir||snprintf(directory,sizeof(directory),"%s%sime/",app_dir,
    app_dir[0]&&app_dir[strlen(app_dir)-1]!='/'&&app_dir[strlen(app_dir)-1]!='\\'?"/":"")>=(int)sizeof(directory)) {
  ime_activate_pack(NULL);return IME_PACK_BOUNDS;
 }
 snprintf(path,sizeof(path),"%sactive.txt",directory);
 if(active_basename(path,name)) {
  snprintf(path,sizeof(path),"%s%s",directory,name);
  r=ime_pack_prepare(path,&p);
  if(!r){ime_activate_pack(p);return 0;}
 }
 snprintf(path,sizeof(path),"%scurrent.bin",directory);
 r=ime_pack_prepare(path,&p);ime_activate_pack(p);return r;
}
void ime_suspend_dictionary(void) {
 if(g_suspended)return;
 char path[sizeof(g_resume_path)];snprintf(path,sizeof(path),"%s",g_resume_path);
 ime_activate_pack(NULL);snprintf(g_resume_path,sizeof(g_resume_path),"%s",path);g_suspended=path[0]!=0;
}
int ime_resume_dictionary(void) {
 ImePack *p=NULL;int r;
 if(!g_suspended)return 0;
 g_suspended=0;
 if(g_pack||!g_resume_path[0])return 0;
 r=ime_pack_prepare(g_resume_path,&p);if(!r)ime_activate_pack(p);return r;
}
unsigned int ime_dictionary_revision(void) { return ime_pack_revision(g_pack); }

int ime_feed(int ch)
{
    int len = (int)strlen(g_pre);

    g_last[0] = 0;

    /* 英文直输：不消费按键，调用方把字母写进搜索框 */
    if (g_mode == IME_ASCII) return 2;

    if (ch == '\b' || ch == 127) {
        ime_backspace();
        return 0;
    }
    if (ch == ' ' || ch == '\n' || ch == '\r') {
        char out[IME_WORD_MAX];

        kana_finish();
        int n = g_cand_n > 0 ? ime_select(0, out, (int)sizeof(out))
                             : ime_commit_raw(out, (int)sizeof(out));
        (void)n;
        return 1;
    }
    if (ch == '-') {
        if (len + 1 < IME_PRE_MAX) {
            g_pre[len] = '-';
            g_pre[len + 1] = 0;
        }
        if (g_mode == IME_KANA) kana_flush_romaji();
        rebuild_candidates();
        return 0;
    }
    if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
    if (ch < 'a' || ch > 'z') return 0;

    if (len + 1 >= IME_PRE_MAX) return 0;
    g_pre[len] = (char)ch;
    g_pre[len + 1] = 0;

    if (g_mode == IME_KANA) kana_flush_romaji();
    rebuild_candidates();
    return 0;
}

void ime_backspace(void)
{
    int len = (int)strlen(g_pre);

    g_last[0] = 0;
    if (g_mode == IME_KANA) {
        /* 先退罗马字，再退假名（UTF-8 安全退一个字符） */
        if (len > 0) {
            g_pre[len - 1] = 0;
        } else {
            int kl = (int)strlen(g_kana);

            if (kl > 0) {
                kl--;
                while (kl > 0 && ((unsigned char)g_kana[kl] & 0xC0) == 0x80) kl--;
                g_kana[kl] = 0;
            }
        }
    } else if (len > 0) {
        g_pre[len - 1] = 0;
    }
    rebuild_candidates();
}

int ime_commit_raw(char *out, int cap)
{
    const char *src;

    kana_finish();
    src = g_mode == IME_KANA ? g_kana : g_pre;

    if (!src[0] || cap <= 1) return 0;
    snprintf(out, (size_t)cap, "%s", src);
    snprintf(g_last, sizeof(g_last), "%s", src);
    ime_reset();
    return (int)strlen(out);
}

int ime_select(int i, char *out, int cap)
{
    if (i < 0 || i >= g_cand_n || cap <= 1) return 0;
    const char *word=ime_candidate(i);
    if(!word[0]||strlen(word)>=(size_t)cap)return 0;
    snprintf(out, (size_t)cap, "%s", word);
    snprintf(g_last, sizeof(g_last), "%s", word);
    if(g_mode==IME_PINYIN&&(g_refs[i].id>>30)==IME_PACK_CHAR) {
        ImePackEntry e;unsigned id=g_refs[i].id&0x3fffffffu;
        if(dictionary_read(IME_PACK_CHAR,(int)id,&e)&&strlen(e.key)<strlen(g_pre)&&!strncmp(g_pre,e.key,strlen(e.key))) {
            memmove(g_pre,g_pre+strlen(e.key),strlen(g_pre)-strlen(e.key)+1);
            rebuild_candidates();
        } else ime_reset();
    } else ime_reset();
    return (int)strlen(out);
}

int ime_assoc(const char *text,char out[][IME_WORD_MAX],int max)
{
 uint32_t scores[32];int n=0,latin=1,lo=0,hi;size_t tl;
 if(!text||!*text||!out||max<=0)return 0;
 if(max>32)max=32;
 if(g_assoc_valid&&g_assoc_max==max&&!strcmp(g_assoc_key,text)) {
  memcpy(out,g_assoc_words,(size_t)g_assoc_n*IME_WORD_MAX);return g_assoc_n;
 }
 tl=strlen(text);for(size_t i=0;i<tl;i++)if((unsigned char)text[i]>=128)latin=0;
 hi=dictionary_count(0);if(latin)dictionary_range(0,text,&lo,&hi);
 for(int kind=0;kind<2;kind++) {
  int begin=kind==0?lo:0,end=kind==0?hi:latin?0:dictionary_count(1);
  for(int i=begin;i<end;i++) {
   ImePackEntry e;int pos,dup=0;if(!dictionary_read(kind,i,&e))continue;
   if((!latin||kind==1)&&strncmp(e.word,text,tl))continue;
   if(!strcmp(e.word,text))continue;
   for(int j=0;j<n;j++)if(!strcmp(out[j],e.word))dup=1;
   if(dup)continue;
   pos=n<max?n:max;
   while(pos>0&&scores[pos-1]<e.freq){if(pos<max){scores[pos]=scores[pos-1];memcpy(out[pos],out[pos-1],IME_WORD_MAX);}pos--;}
   if(pos<max){scores[pos]=e.freq;snprintf(out[pos],IME_WORD_MAX,"%s",e.word);if(n<max)n++;}
  }
 }
 if(tl<sizeof(g_assoc_key)) {
  snprintf(g_assoc_key,sizeof(g_assoc_key),"%s",text);g_assoc_max=max;g_assoc_n=n;g_assoc_valid=1;
  memcpy(g_assoc_words,out,(size_t)n*IME_WORD_MAX);
 }
 return n;
}
