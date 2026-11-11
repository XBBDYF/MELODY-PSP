/* Fixed-memory local MP3 player. No API/cache/skin runtime dependencies. */
#include "ultra_player.h"
#include "app.h"
#include "audio.h"
#include "local.h"
#include "gfx.h"
#include "log.h"
#include "cache.h"
#include "coverjpeg.h"
#include "power_runtime.h"
#include "json.h"
#include <stdlib.h>
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define ULTRA_LINES 90
#define ULTRA_TEXT 96
#define ULTRA_PATH 256
#define ULTRA_READ_LIMIT (64 * 1024)

enum { PAGE_LIST, PAGE_PLAYER, PAGE_LYRICS, PAGE_SETTINGS };
static struct {
    int page, count, selected, playing, armed, error;
    int tab, playing_tab, playing_count, cache_count, cache_indices[256], options, option_row, lyrics_on,cover_on,clock_index;
    int cover_w,cover_h,cache_loaded,cache_scan_pos,cache_scan_total;
    char cache_names[256][72];
    unsigned char cover_rgba[96*96*4];
    int lyric_count, lyric_ms[ULTRA_LINES];
    char lyric[ULTRA_LINES][ULTRA_TEXT];
} u;

static void clear_now(void)
{
    memset(&g_app.now, 0, sizeof(g_app.now));
    g_app.has_now = 0;
    g_app.paused = 0;
    g_app.playing_index = -1;
    g_app.is_liked = 0;
    g_app.menu_open = 0;
    g_app.play_all_next = 0;
}

static void page(int value)
{
    u.page = value;
    g_app.screen = value == PAGE_LIST ? SC_CACHE :
        value == PAGE_PLAYER ? SC_PLAYER : value == PAGE_LYRICS ? SC_LYRICS : SC_SETTINGS;
    g_app.cache_tab = 1;
    g_app.lyrics_open = value == PAGE_LYRICS;
}

/* fgets alone can split a long physical line into false timestamp records.
 * Consume its tail, and cap both bytes read and stored records. No heap. */
static void load_lyrics(const char *path)
{
    char file[ULTRA_PATH], line[512];
    char *dot;
    FILE *fp;
    int budget = ULTRA_READ_LIMIT;
    size_t length = strlen(path);
    u.lyric_count = 0;
    if (length >= sizeof(file)) return;
    memcpy(file, path, length + 1);
    dot = strrchr(file, '.');
    if (!dot || (size_t)(dot - file) + 5 > sizeof(file)) return;
    strcpy(dot, ".lrc");
    io_lock();
    fp = fopen(file, "rb");
    if (!fp){io_unlock();return;}
    while (budget > 0 && u.lyric_count < ULTRA_LINES && fgets(line, sizeof(line), fp)) {
        char *text = line;
        int times[ULTRA_LINES], n = 0, minute, second, end;
        size_t size = strlen(line);
        budget -= (int)size;
        if (size && line[size - 1] != '\n' && !feof(fp)) {
            int ch;
            while (budget-- > 0 && (ch = fgetc(fp)) != EOF && ch != '\n') { }
        }
        if ((unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb &&
            (unsigned char)text[2] == 0xbf) text += 3;
        while (*text == '[' && n < ULTRA_LINES) {
            char *close = strchr(text, ']');
            int fraction = 0, scale = 100;
            if (!close || sscanf(text, "[%d:%d%n", &minute, &second, &end) != 2 ||
                minute < 0 || minute > 35790 || second < 0 || second > 59) break;
            {
                char *p = text + end;
                if (*p == '.') {
                    p++;
                    while (p < close && *p >= '0' && *p <= '9') {
                        if (scale) { fraction += (*p - '0') * scale; scale /= 10; }
                        p++;
                    }
                }
                if (p != close) break;
            }
            times[n++] = minute * 60000 + second * 1000 + fraction;
            text = close + 1;
        }
        text[strcspn(text, "\r\n")] = 0;
        if (!*text) continue;
        for (int i = 0; i < n && u.lyric_count < ULTRA_LINES; ++i) {
            int at = u.lyric_count++;
            /* Stable insertion handles multi-tags and unsorted local LRC. */
            while (at > 0 && u.lyric_ms[at - 1] > times[i]) {
                u.lyric_ms[at] = u.lyric_ms[at - 1];
                memcpy(u.lyric[at], u.lyric[at - 1], ULTRA_TEXT);
                at--;
            }
            u.lyric_ms[at] = times[i];
            snprintf(u.lyric[at], ULTRA_TEXT, "%.*s", ULTRA_TEXT - 1, text);
        }
    }
    fclose(fp);
    io_unlock();
}

static void scan_cache_step(void){
 CacheEntry entries[8];
 if(u.cache_scan_pos>=u.cache_scan_total||u.cache_count>=256)return;
 int first=u.cache_scan_pos,n=cache_snapshot_page(entries,8,first);
 if(n<=0){u.cache_scan_pos=u.cache_scan_total;return;}
 /* snapshot_page reads the complete-cache index itself; cache_complete would reopen and search that same index for every row. */
 for(int i=0;i<n&&u.cache_count<256;++i)if(entries[i].id>0){
  int at=u.cache_count++;u.cache_indices[at]=first+i;
  snprintf(u.cache_names[at],sizeof(u.cache_names[at]),"%s",entries[i].name);
 }
 u.cache_scan_pos+=n;
 if(u.tab)u.count=u.cache_count;
}
static void scan_cache(void){
 u.cache_loaded=1;u.cache_count=0;u.cache_scan_pos=0;u.cache_scan_total=cache_count();
 scan_cache_step();
}
static void load_cover(const char *audio_path){
 u.cover_w=u.cover_h=0;
 if(!u.cover_on)return;
 char path[256];
 if(u.tab)cache_path(path,sizeof(path),2,g_app.now.id);
 else {snprintf(path,sizeof(path),"%s",audio_path);char*dot=strrchr(path,'.');if(!dot||dot-path+5>=(int)sizeof(path))return;strcpy(dot,".jpg");}
 unsigned char *b=malloc(128*1024);if(!b)return;
 io_lock();FILE*f=fopen(path,"rb");int n=0;if(f){n=(int)fread(b,1,128*1024,f);if(!feof(f))n=0;fclose(f);}io_unlock();
 if(n>0)coverjpeg_decode_rgba_scaled(b,n,u.cover_rgba,sizeof(u.cover_rgba),96,&u.cover_w,&u.cover_h);
 free(b);
}
static void lyric_json(void *ud,const char *path,int type,const char *text,long long number){
 (void)ud;(void)number;if(type!=JSON_STR||strcmp(path,"lrc.lyric"))return;
 u.lyric_count=0;const char*p=text;
 while(*p&&u.lyric_count<ULTRA_LINES){int m,sec,end=0;if(sscanf(p,"[%d:%d%n",&m,&sec,&end)==2&&m>=0&&m<35790&&sec>=0&&sec<60){const char*close=strchr(p,']');if(close){const char*nl=strchr(close+1,'\n');int len=nl?(int)(nl-close-1):(int)strlen(close+1);if(len>=ULTRA_TEXT)len=ULTRA_TEXT-1;int i=u.lyric_count++;u.lyric_ms[i]=m*60000+sec*1000;memcpy(u.lyric[i],close+1,(size_t)len);u.lyric[i][len]=0;}}const char*nl=strchr(p,'\n');if(!nl)break;p=nl+1;}
}
static void load_cached_lyrics(long long id){
 char *json=malloc(65536);if(!json)return;int n=cache_lyric_read(id,json,65535);if(n>0){json[n]=0;json_walk(json,n,lyric_json,NULL);}free(json);
}
static void play(int index)
{
    const char *path, *name;
    if (u.count <= 0 || index < 0 || index >= u.count) return;
    char cached_path[256];CacheEntry cached;memset(&cached,0,sizeof(cached));
    if(u.tab){const CacheEntry*e=cache_get(u.cache_indices[index]);if(!e)return;cached=*e;cache_path(cached_path,sizeof(cached_path),0,cached.id);path=cached_path;name=cached.name;}
    else {path = local_path_of(index);name = local_name(index);}
    audio_stop();
    clear_now();
    u.armed = 0;
    u.error = 0;
    u.lyric_count = 0;
    u.playing = -1;
    u.selected = index;
    page(PAGE_PLAYER);
    if (!path || !*path) { u.error = 1; return; }
    snprintf(g_app.now.name, sizeof(g_app.now.name), "%s", name ? name : "MP3");
    snprintf(g_app.now.artist, sizeof(g_app.now.artist), "%s", "本地音乐");
    g_app.now.playable = 1;
    if(u.tab){g_app.now.id=cached.id;g_app.now.dur_ms=cached.dur_ms;snprintf(g_app.now.artist,sizeof(g_app.now.artist),"%s",cached.artist);}
    if (audio_play_file(path, &g_app.now) < 0) {
        clear_now();
        u.error = 1;
        return;
    }
    u.playing = index;
    u.playing_tab=u.tab;u.playing_count=u.count;
    u.armed = 1;
    g_app.has_now = 1;
    g_app.playing_index = index;
    g_app.local_sel = index;
    g_app.play_from = SC_CACHE;
    g_app.play_from_tab = 1;
    load_lyrics(path);
    if(u.tab)load_cached_lyrics(g_app.now.id);
    load_cover(path);
}

void ultra_player_init(void)
{
    memset(&u, 0, sizeof(u));
    u.playing = -1;u.lyrics_on=1;
}

void ultra_player_enter(void)
{
    audio_stop();
    ultra_player_init();
    clear_now();
    u.count = local_count(); /* local_count owns the existing one-time scan. */
    if (u.count < 0) u.count = 0;
    g_app.local_sel = 0;
    page(PAGE_LIST);
}

int ultra_player_tick(unsigned int pressed, unsigned int held, float dt)
{
    int state = audio_state();
    if(u.tab&&u.page==PAGE_LIST&&u.cache_loaded)scan_cache_step();
    if(pressed&PSP_CTRL_START){u.options=!u.options;u.option_row=0;return 0;}
    if(u.options){
      if(pressed&PSP_CTRL_CROSS){u.options=0;return 0;}
      if((pressed&PSP_CTRL_UP)&&u.option_row>0)--u.option_row;
      if((pressed&PSP_CTRL_DOWN)&&u.option_row<2)++u.option_row;
      if(pressed&(PSP_CTRL_CIRCLE|PSP_CTRL_LEFT|PSP_CTRL_RIGHT)){
        if(!u.option_row)u.lyrics_on=!u.lyrics_on;
        else if(u.option_row==1){u.cover_on=!u.cover_on;const char*path=u.tab?"":(u.playing>=0?local_path_of(u.playing):"");load_cover(path?path:"");}
        else {static const int clocks[]={133,100,80,66};u.clock_index=(u.clock_index+1)%4;power_runtime_set_ultra_clock(clocks[u.clock_index]);}
      }
      return 0;
    }
    if(u.page==PAGE_LIST&&(pressed&(PSP_CTRL_SQUARE|PSP_CTRL_LEFT|PSP_CTRL_RIGHT))){u.tab=!u.tab;if(u.tab&&!u.cache_loaded)scan_cache();u.selected=0;u.count=u.tab?u.cache_count:local_count();return 0;}

    (void)held;
    (void)dt;
    if (pressed & PSP_CTRL_SELECT) { page(PAGE_SETTINGS); return 0; }
    if (pressed & PSP_CTRL_CROSS) { page(PAGE_LIST); return 0; }
    if (u.page == PAGE_SETTINGS) return !!(pressed & PSP_CTRL_CIRCLE);
    if ((pressed & PSP_CTRL_TRIANGLE)&&u.lyrics_on) page(u.page == PAGE_LYRICS ? PAGE_PLAYER : PAGE_LYRICS);
    if ((pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) && u.count > 0) {
        if(u.playing>=0&&u.playing_tab!=u.tab){u.tab=u.playing_tab;u.count=u.playing_count;}
        int next = u.playing >= 0 ? u.playing : u.selected;
        next += (pressed & PSP_CTRL_RTRIGGER) ? 1 : -1;
        if (next < 0) next = u.count - 1;
        if (next >= u.count) next = 0;
        play(next);
        return 0;
    }
    if (pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        int delta = (pressed & PSP_CTRL_UP) ? -1 : 1;
        if (u.page == PAGE_LIST) {
            if (u.count > 0) {
                u.selected += delta;
                if (u.selected < 0) u.selected = u.count - 1;
                if (u.selected >= u.count) u.selected = 0;
                g_app.local_sel = u.selected;
            }
        } else {
            int volume = audio_volume() - delta * 5;
            if (volume < 0) volume = 0;
            if (volume > 100) volume = 100;
            g_volume = audio_set_volume(volume);
        }
    }
    if (pressed & PSP_CTRL_CIRCLE) {
        if (u.page == PAGE_LIST || !g_app.has_now) play(u.selected);
        else if (state == AUDIO_ERROR || state == AUDIO_END || state == AUDIO_IDLE) play(u.selected);
        else { audio_pause(!audio_paused()); g_app.paused = audio_paused(); }
        return 0;
    }
    if (u.armed && state == AUDIO_ERROR) {
        u.armed = 0; /* Never skip/retry every frame on a broken decoder/file. */
        u.error = 1;
    } else if (u.armed && state == AUDIO_END) {
        u.armed = 0;
        if(u.playing_tab!=u.tab){u.tab=u.playing_tab;u.count=u.playing_count;}
        if (u.count > 0) play(u.playing + 1 < u.count ? u.playing + 1 : 0);
    }
    return 0;
}

static void text(int y, const char *value, int bright)
{
    gfx_text_ellipsis(FS_MD, 18, y, 444, value ? value : "", bright ? 0xffff : 0xad55);
}

void ultra_player_draw(void)
{
    char status[96];
    gfx_clear(0);
    text(12, "MELODY · 极致省电 / 本地 MP3", 1);
    if(u.options){
        char opt[96];text(48,"播放器选项（仅影响当前皮肤）",1);
        snprintf(opt,sizeof(opt),"%s歌词：%s",u.option_row==0?"> ":"  ",u.lyrics_on?"开":"关");text(90,opt,u.option_row==0);
        snprintf(opt,sizeof(opt),"%s离线封面：%s",u.option_row==1?"> ":"  ",u.cover_on?"开":"关");text(120,opt,u.option_row==1);
        static const int clocks[]={133,100,80,66};snprintf(opt,sizeof(opt),"%sCPU目标：%dMHz（低频稳定性待实机验证）",u.option_row==2?"> ":"  ",clocks[u.clock_index]);text(150,opt,u.option_row==2);
        text(240,"↑↓ 选择  ○调整  START/× 返回",0);return;
    }
    if (u.page == PAGE_SETTINGS) {
        text(72, "> 退出当前省电皮肤", 1);
        text(232, "○ 确认退出    × 本地列表", 0);
        return;
    }
    if (u.page == PAGE_LIST) {
        int first = (u.selected / 7) * 7;
        text(40, u.tab?(u.cache_scan_pos<u.cache_scan_total&&u.cache_count<256?"正在分批读取缓存索引…":"完整缓存（□切本地）"):"本地音乐（□切完整缓存）", 0);
        if (!u.count) text(90, "暂无本地 MP3，请放入 MUSIC 目录", 1);
        for (int i = first; i < u.count && i < first + 7; ++i) {
            if (i == u.selected) gfx_fill_rect(10, 65 + (i-first)*22, 460, 21, 0x2104);
            text(66 + (i-first)*22,u.tab?u.cache_names[i]:local_name(i),i==u.selected);
        }
        text(246, "↑↓选歌 ○播放 □本地/缓存 START选项 SELECT退出", 0);
        return;
    }
    text(48, g_app.has_now ? g_app.now.name : "尚未播放", 1);
    if (u.error || audio_state() == AUDIO_ERROR) {
        text(86, "播放失败：○ 重试，L/R 切换本地歌曲", 1);
        text(110, audio_error(), 0);
    } else if (u.page == PAGE_LYRICS && u.lyrics_on) {
        int current = -1, pos = audio_pos_ms();
        for (int i = 0; i < u.lyric_count && u.lyric_ms[i] <= pos; ++i) current = i;
        if (!u.lyric_count) text(120, "无同名本地 .lrc 歌词", 0);
        else if (current < 0) text(144, u.lyric[0], 0);
        else {
            if (current > 0) text(94, u.lyric[current-1], 0);
            text(130, u.lyric[current], 1);
            if (current+1 < u.lyric_count) text(166, u.lyric[current+1], 0);
        }
    } else {
        int seconds = audio_pos_ms() / 1000, duration = audio_dur_ms() / 1000;
        if (seconds < 0) seconds = 0;
        if (duration < 0) duration = 0;
        snprintf(status, sizeof(status), "%s   %d:%02d / %d:%02d   音量 %d",
            audio_paused() ? "暂停" : audio_state() == AUDIO_BUFFER ? "读取中" : "播放",
            seconds/60, seconds%60, duration/60, duration%60, audio_volume());
        text(112, status, 1);
    }
    if(u.cover_on&&u.cover_w>0&&u.page!=PAGE_LYRICS)gfx_blit_rgba_scaled(365,65,96,96,u.cover_rgba,u.cover_w,u.cover_h,255);
    text(222, "○ 播放/暂停  ↑↓ 音量  L/R 上/下一首", 0);
    text(246, "△ 歌词 ×列表 START歌词/封面/CPU SELECT退出", 0);
}

int ultra_player_needs_ui_clock(void){
 return u.page==PAGE_LIST||u.page==PAGE_SETTINGS||u.options||audio_state()==AUDIO_BUFFER;
}
