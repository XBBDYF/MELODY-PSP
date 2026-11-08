#include "store.h"

#include "log.h"
#include "search_history.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORE_PATH APP_DIR "data.txt"
#define MAX_RECENT 20
#define MAX_FAV    40

static Song g_recent[MAX_RECENT];
static int g_recent_n;
static Song g_fav[MAX_FAV];
static int g_quality;
static int g_show_mem = 1;
static int g_show_speed=1;
static int g_prefetch_count=1;
static int g_fav_n;
static int g_theme;              /* 0 = 浅色 / 1 = 深色 */
static int g_renderer;           /* E: 0 = CPU (legacy default), 1 = GE / GU */
static int g_disc_player;        /* V: opt-in rotating disc; legacy cover by default */
/* L: also scan ms0:/VIDEO for local videos. Off by default — the MV list is meant
 * to show what this app downloaded; a big VIDEO folder is slow to stat and can
 * contain files the built-in decoder cannot play. */
static int g_scan_local_mv;
static int g_dl_parts = 4;       /* 下载并发连接数，和 dl.c 的 DL_MAX_PARTS 一致 */

/* 去掉字段里的分隔符 */
static void clean_field(char *s)
{
    for (; *s; ++s) {
        if (*s == '|' || *s == '\n' || *s == '\r') *s = ' ';
    }
}

static int split6(char *line, char *f[6])
{
    int n = 0;
    char *p = line;

    f[n++] = p;
    while (*p && n < 6) {
        if (*p == '|') {
            *p = 0;
            f[n++] = p + 1;
        }
        p++;
    }
    return n;
}

void store_init(void)
{
    FILE *fp;
    char line[420];

    g_recent_n = g_fav_n = 0;
    g_renderer = 0; /* Old data.txt files retain CPU rendering. */
    g_disc_player = 0; /* Missing V records retain the original square cover. */
    g_scan_local_mv = 0; /* Missing L records keep local VIDEO scanning off. */

    io_lock();
    fp = fopen(STORE_PATH, "r");
    io_unlock();
    if (!fp) return;

    while (fgets(line, sizeof(line), fp)) {
        char *f[6];
        char kind;
        Song s;
        int nf;

        nf = split6(line, f);
        if (nf < 5) continue;
        kind = f[0][0];
        memset(&s, 0, sizeof(s));
        s.id = atoll(f[1]);
        s.dur_ms = atoi(f[2]);
        snprintf(s.name, sizeof(s.name), "%s", f[3]);
        snprintf(s.artist, sizeof(s.artist), "%s", f[4]);
        if (nf >= 6) s.mv = atoi(f[5]);
        clean_field(s.name);
        clean_field(s.artist);
        if (!s.name[0]) continue;

        if (kind == 'R' && g_recent_n < MAX_RECENT) g_recent[g_recent_n++] = s;
        else if (kind == 'F' && g_fav_n < MAX_FAV) g_fav[g_fav_n++] = s;
        else if (kind == 'Q') g_quality = atoi(f[1]);
        else if (kind == 'M') g_show_mem = atoi(f[1]);
        else if(kind=='N')g_show_speed=atoi(f[1])!=0;
        else if(kind=='B'){int n=atoi(f[1]);g_prefetch_count=n<0?0:n>5?5:n;}
        /* 修复：新增主题 / 下载连接数两行；老 data.txt 没有这两行时用默认值 */
        else if (kind == 'T') g_theme = atoi(f[1]) ? 1 : 0;
        else if (kind == 'E') g_renderer = atoi(f[1]) == 1 ? 1 : 0;
        else if (kind == 'V') g_disc_player = strcmp(f[1], "1") == 0;
        else if (kind == 'L') g_scan_local_mv = strcmp(f[1], "1") == 0;
        else if (kind == 'P') {
            int parts = atoi(f[1]);

            if (parts >= 2 && parts <= 6) g_dl_parts = parts;
        }
        else if (kind == 'H') {
            search_hist_push(f[1]);          /* 文件里的顺序就是新→旧 */
        }
    }

    fclose(fp);
    mlog("store: %d recent, %d fav", g_recent_n, g_fav_n);
}

void store_save(void)
{
    FILE *fp;

    io_lock();
    fp = fopen(STORE_PATH, "w");
    if (!fp) {
        io_unlock();
        return;
    }

    for (int i = 0; i < g_recent_n; ++i) {
        char n[80], a[80];
        snprintf(n, sizeof(n), "%s", g_recent[i].name);
        snprintf(a, sizeof(a), "%s", g_recent[i].artist);
        clean_field(n);
        clean_field(a);
        fprintf(fp, "R|%lld|%d|%s|%s|%d\n", g_recent[i].id, g_recent[i].dur_ms,
                n, a, g_recent[i].mv);
    }
    {
        char hist[SEARCH_HIST_MAX][SEARCH_HIST_LEN];
        int n = search_hist_export(hist, SEARCH_HIST_MAX);

        for (int i = 0; i < n; ++i) {
            char q[SEARCH_HIST_LEN];

            memcpy(q, hist[i], sizeof(q));
            q[sizeof(q) - 1] = 0;
            clean_field(q);
            if (q[0]) fprintf(fp, "H|%s\n", q);
        }
    }
    for (int i = 0; i < g_fav_n; ++i) {
        char n[80], a[80];
        snprintf(n, sizeof(n), "%s", g_fav[i].name);
        snprintf(a, sizeof(a), "%s", g_fav[i].artist);
        clean_field(n);
        clean_field(a);
        fprintf(fp, "F|%lld|%d|%s|%s|%d\n", g_fav[i].id, g_fav[i].dur_ms,
                n, a, g_fav[i].mv);
    }

    fprintf(fp, "Q|%d|0|-|-\n", g_quality);
    fprintf(fp, "M|%d|0|-|-\n", g_show_mem);
    fprintf(fp,"N|%d|0|-|-\n",g_show_speed);
    fprintf(fp,"B|%d|0|-|-\n",g_prefetch_count);
    /* 修复：主题 / 下载连接数也落盘（格式：T|0|0|-|- 、P|4|0|-|-） */
    fprintf(fp, "T|%d|0|-|-\n", g_theme ? 1 : 0);
    fprintf(fp, "E|%d|0|-|-\n", g_renderer);
    fprintf(fp, "V|%d|0|-|-\n", g_disc_player);
    fprintf(fp, "L|%d|0|-|-\n", g_scan_local_mv);
    fprintf(fp, "P|%d|0|-|-\n", g_dl_parts);
    fclose(fp);
    io_unlock();
}

int store_quality(void) { return g_quality; }
void store_set_quality(int q) { g_quality = q; store_save(); }
int store_show_mem(void) { return g_show_mem; }
void store_set_show_mem(int on) { g_show_mem = on ? 1 : 0; store_save(); }

int store_theme(void) { return g_theme; }
void store_set_theme(int theme) { g_theme = theme ? 1 : 0; store_save(); }

int store_renderer(void) { return g_renderer; }
void store_set_renderer(int renderer) { g_renderer = renderer == 1 ? 1 : 0; store_save(); }

int store_disc_player(void) { return g_disc_player; }void store_set_disc_player(int on) { g_disc_player = on == 1 ? 1 : 0; store_save(); }

int store_scan_local_mv(void) { return g_scan_local_mv; }
void store_set_scan_local_mv(int on) { g_scan_local_mv = on == 1 ? 1 : 0; store_save(); }

int store_dl_parts(void) { return g_dl_parts; }
void store_set_dl_parts(int parts)
{
    if (parts < 2) parts = 2;
    if (parts > 6) parts = 6;
    g_dl_parts = parts;
    store_save();
}

const Song *store_recent_get(int i)
{
    if (i < 0 || i >= g_recent_n) return NULL;
    return &g_recent[i];
}

int store_recent_count(void) { return g_recent_n; }

const Song *store_recent(int i)
{
    if (i < 0 || i >= g_recent_n) return NULL;
    return &g_recent[i];
}

void store_push_recent(const Song *s)
{
    int found = -1;

    if (!s || !s->name[0]) return;

    for (int i = 0; i < g_recent_n; ++i) {
        if (g_recent[i].id == s->id) { found = i; break; }
    }

    if (found == 0) return;

    if (found > 0) {
        memmove(&g_recent[1], &g_recent[0], (size_t)found * sizeof(Song));
        g_recent[0] = *s;
    } else {
        if (g_recent_n < MAX_RECENT) g_recent_n++;
        memmove(&g_recent[1], &g_recent[0], (size_t)(g_recent_n - 1) * sizeof(Song));
        g_recent[0] = *s;
    }

    store_save();
}

void store_clear_recent(void)
{
    g_recent_n = 0;
    store_save();
}

int store_fav_count(void) { return g_fav_n; }

const Song *store_fav(int i)
{
    if (i < 0 || i >= g_fav_n) return NULL;
    return &g_fav[i];
}

int store_is_fav(long long id)
{
    for (int i = 0; i < g_fav_n; ++i)
        if (g_fav[i].id == id) return 1;
    return 0;
}

/* 修复：以前返回 void，收藏满了也当成功（UI 照样提示「已收藏」）。
 * 现在返回 0 = 已添加 / 1 = 已移除 / 2 = 收藏已满。C 允许老调用者忽略返回值。 */
int store_toggle_fav(const Song *s)
{
    int found = -1;

    if (!s) return 2;

    for (int i = 0; i < g_fav_n; ++i) {
        if (g_fav[i].id == s->id) { found = i; break; }
    }

    if (found >= 0) {
        memmove(&g_fav[found], &g_fav[found + 1], (size_t)(g_fav_n - found - 1) * sizeof(Song));
        g_fav_n--;
        store_save();
        return 1;
    }

    if (g_fav_n < MAX_FAV) {
        g_fav[g_fav_n++] = *s;
        store_save();
        return 0;
    }

    mlog("store: fav full (%d)", MAX_FAV);
    return 2;
}

#define PLAYLIST_FAV_PATH APP_DIR "playlist-favorites.bin"
static Card playlist_favs[MAX_CARDS];static int playlist_fav_n,playlist_fav_loaded;
static void playlist_fav_load(void){
    if(playlist_fav_loaded)return;playlist_fav_loaded=1;
    SceUID fd=sceIoOpen(PLAYLIST_FAV_PATH,PSP_O_RDONLY,0);if(fd<0)fd=sceIoOpen(PLAYLIST_FAV_PATH ".old",PSP_O_RDONLY,0);if(fd<0)return;
    unsigned header[3];int n=sceIoRead(fd,header,sizeof(header));
    if(n==(int)sizeof(header)&&header[0]==0x504c4631u&&header[1]==sizeof(Card)&&header[2]<=MAX_CARDS){
        unsigned bytes=header[2]*sizeof(Card);if(sceIoRead(fd,playlist_favs,bytes)==(int)bytes)playlist_fav_n=(int)header[2];
    }sceIoClose(fd);
    for(int i=0;i<playlist_fav_n;++i){playlist_favs[i].name[63]=0;playlist_favs[i].cover[519]=0;playlist_favs[i].sub[47]=0;if(playlist_favs[i].id<=0){playlist_fav_n=0;break;}}
}
int store_playlist_has(long long id){playlist_fav_load();for(int i=0;i<playlist_fav_n;++i)if(playlist_favs[i].id==id)return 1;return 0;}
int store_playlist_snapshot(Card*out,int max){playlist_fav_load();int n=playlist_fav_n<max?playlist_fav_n:max;if(out&&n>0)memcpy(out,playlist_favs,(size_t)n*sizeof(Card));return n;}
int store_playlist_toggle(const Card*card){
    if(!card||card->id<=0||card->kind==2||card->kind==3)return -1;
    playlist_fav_load();Card old[MAX_CARDS];memcpy(old,playlist_favs,sizeof(old));int old_n=playlist_fav_n,removed=0;
    for(int i=0;i<playlist_fav_n;++i)if(playlist_favs[i].id==card->id){memmove(playlist_favs+i,playlist_favs+i+1,(size_t)(playlist_fav_n-i-1)*sizeof(Card));--playlist_fav_n;removed=1;break;}
    if(!removed){if(playlist_fav_n==MAX_CARDS)return 2;playlist_favs[playlist_fav_n++]=*card;}
    unsigned header[3]={0x504c4631u,sizeof(Card),(unsigned)playlist_fav_n};
    SceUID fd=sceIoOpen(PLAYLIST_FAV_PATH ".tmp",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0600);int okay=fd>=0;
    if(okay){okay=sceIoWrite(fd,header,sizeof(header))==(int)sizeof(header);unsigned bytes=playlist_fav_n*sizeof(Card);if(okay)okay=sceIoWrite(fd,playlist_favs,bytes)==(int)bytes;if(sceIoClose(fd)<0)okay=0;}
    if(okay){sceIoRemove(PLAYLIST_FAV_PATH ".old");int had_old=sceIoRename(PLAYLIST_FAV_PATH,PLAYLIST_FAV_PATH ".old")>=0;okay=sceIoRename(PLAYLIST_FAV_PATH ".tmp",PLAYLIST_FAV_PATH)>=0;if(!okay&&had_old)sceIoRename(PLAYLIST_FAV_PATH ".old",PLAYLIST_FAV_PATH);}
    if(!okay){memcpy(playlist_favs,old,sizeof(old));playlist_fav_n=old_n;return -1;}
    return removed?1:0;
}

int store_show_speed(void){return g_show_speed;}
void store_set_show_speed(int on){g_show_speed=on!=0;store_save();}

int store_prefetch_count(void){return g_prefetch_count;}
void store_set_prefetch_count(int n){g_prefetch_count=n<0?0:n>5?5:n;store_save();}
