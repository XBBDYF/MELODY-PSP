/*
 * MELODY PSP — 网易云 HTTP 接口 + 后台请求线程
 *
 * 走的是网易云还在的「明文 HTTP」老接口（PSP 没有 TLS，只能用这些）：
 *   /api/v3/playlist/detail      榜单（返回 20 首，约 80KB）
 *   /api/cloudsearch/pc          搜索（返回 ar/al/dt/fee，和榜单同构）
 *   /api/song/enhance/player/url 取 mp3 直链（http）
 *   /api/song/lyric              歌词
 * 封面图也走 http（p1.music.126.net），?param=200y200 缩到 200x200 的 JPEG。
 */

#ifndef MELODY_API_H
#define MELODY_API_H

#include "app.h"

#define API_BASE "http://music.163.com"

void api_init(void);
int api_request_skin(int operation,const char*id); /*0 catalog,1 install,2 preview*/
int api_skin_busy(void);
void api_free_buffers(void);
/* 返回 1 = 线程被强杀（可能正握着文件锁，调用方不要再写文件） */
int api_shutdown(void);

/* 当前音质对应的 br（128k/192k/320k） */
int api_quality_br(void);
void api_note_play_fail(void);   /* 播放失败：下次降级到 128k */
void api_request_like(long long id, int like);
unsigned int api_account_generation(void);
unsigned int api_like_generation(void);
void api_request_subscribe(long long id, int sub);
void api_note_play_ok(void);     /* 播放成功：恢复正常音质 */

int api_chart_count(void);
const Chart *api_chart(int index);

/* MV 详情（网易云 MV id → 名称/封面/240p 直链）。直链带签名，会过期。 */
typedef struct {
    int id;
    int state;          /* 0 未请求 1 加载中 2 就绪 3 失败 */
    char name[72];
    char artist[64];
    char cover[208];
    int dur_ms;
    char url[900];      /* 240p MP4，带 wsSecret/wsTime */
} MvInfo;

void api_request_announcement_check(void);
void api_request_announcement_dismiss(int ignore);
void api_request_mv_detail(int mvid);
void api_request_mv_download(int mvid);
int api_request_mv_save(int mvid);
void api_mv_download_abort(void);
int api_mv_download_state(void);   /* 0 idle 1 downloading 2 done 3 fail */
int api_mv_download_pct(void);
long long api_mv_download_total(void);
/* Bytes on disk for the MV being fetched; always <= total. Display this instead
 * of recomputing bytes from the percent. */
long long api_mv_download_bytes(void);
const MvInfo *api_mv_info(void);

/* 投递任务（主线程） */
void api_request_probe(void);
void api_request_connect(int profile);
void api_request_chart(int chart_index);
void api_request_search(const char *query);
void api_request_search_category(const char *query,int kind);
void api_request_search_open(int kind,long long id);
const CardList *api_search_entities(void);
const char *api_search_category_name(int kind);
void api_request_search_more(void);        /* 加载下一页搜索结果 */
int api_list_total(void);                  /* 上次搜索命中总数 */
void api_request_suggest(const char *query);   /* 搜索联想（拼音 -> 候选） */
int api_suggest_count(void);
const char *api_suggest(int i);
void api_request_check(void);
void api_request_play(int index, const Song *song);
void api_request_lyric(const Song *song);
void api_clear_lyric(void);
/* 1 only for real YRC word timing; byte offsets are UTF-8 boundaries.
 * completed..word_end is the active word, fraction is its elapsed 0..1.
 * Gaps freeze the completed prefix. 0 means use ordinary line highlighting. */
int api_lyric_word_span(int line,int pos_ms,int *completed_bytes,int *word_end_bytes,float *fraction);
int api_lyric_timing(int line,int pos_ms,int *completed_bytes,float *next_fraction);
int api_local_art(const char *path);   /* 读本地 mp3 内嵌封面 */
int api_local_meta(const char *path, Song *song);   /* 读 ID3 标题/歌手 */
void api_request_export(const Song *song);   /* 导出到 ms0:/MUSIC */
void api_request_art(const Song *song);
void api_request_download(const Song *song);
int api_request_download_batch(const Song*songs,int count);
int api_download_pending(long long id);
int api_pending_download_count(void);
void api_clear_pending_downloads(void);
void api_cancel_pending_prefetch(void);
void api_request_prefetch(const Song*song);
/* 本地 mp3 没有内嵌封面/歌词时：按歌名+歌手+时长去网易搜一次，补上封面和歌词。
 * dur_ms 是本地文件实际时长（0 = 不知道），用来排除翻唱/Live/碎片匹配。 */
void api_request_local_meta(const char *name, const char *artist, int dur_ms);
/* 文件名就是网易云 id（<id>.mp3）：一次详情请求直接拿封面+歌词，不搜索 */
void api_request_local_id(long long id);

/* ---------------- 在线更新 ---------------- */
void api_request_update_check(void);
void api_request_update_download(void);
void api_update_abort(void);
int api_update_state(void); /* 7: download queued, 4: receiving */        /* 0 idle 1 checking 2 latest 3 available 4 downloading 5 done 6 error */
int api_update_progress(void);     /* 0..100 */
const char *api_update_version(void);
const char *api_update_changelog(void);   /* 清单第 4 行起的更新日志（\n 分隔） */
const char *api_update_message(void);
void api_update_set_pending(int p);   /* EBOOT.NEW 已下载，等下次启动替换 */
int api_update_pending(void);

/* ---------------- 扫码登录 ---------------- */
void api_request_qr_new(void);       /* 取二维码 */
void api_request_qr_poll(void);      /* 轮询扫码状态 */
void api_logout(void);
void api_request_account(void);      /* 拉账号信息（昵称/VIP） */
int api_qr_state(void);              /* 800 过期 / 801 待扫 / 802 待确认 / 803 成功 */
const char *api_qr_key(void);
const char *api_qr_url(void);
const unsigned char *api_qr_matrix(int *dim);   /* 1 bit/模块，行优先 */
int api_logged_in(void);
const char *api_login_name(void);
long long api_login_uid(void);
int api_login_vip(void);

/* ---------------- 云端内容（歌单 / 每日推荐 / 评论） ---------------- */
void api_request_daily(void);
/* Idle-only, account-owned cover metadata; silent on failure, retry after 60s.
 * Does not publish a list event. Request thumbnails separately using preview art. */
void api_request_daily_preview(void);
const Song*api_daily_preview(void);            /* 每日推荐歌曲 */
void api_request_recommend_lists(void);  /* 推荐歌单（卡片） */
void api_request_square(void);           /* 社区·精选歌单（卡片） */
void api_request_my_lists(void);         /* 我的歌单（卡片） */
void api_request_toplists(void);         /* 排行榜（卡片） */
void api_request_artist_top(void);       /* 歌手榜（卡片） */
void api_request_cat_lists(const char *cat);
/* Discovery: -1 shares homepage recommendations; 0..7 select categories.
 * Stable through playlist/album/artist detail loads; valid until API buffers free. */
void api_request_discovery(int category);
const CardList *api_discovery_cards(void);
void api_request_playlist(long long id); /* 歌单详情 -> 歌曲列表 */
void api_request_playlist_more(int offset);  /* 歌单续页 */
int api_playlist_count(void);            /* 当前歌单的 trackIds 总数（调试用） */
int api_liked_total(void);               /* 我喜欢的音乐云端曲目数（0 = 还没加载过） */
void api_request_liked(void);
void api_request_like_state(void);
int api_song_liked(long long id);
void api_request_saved_playlists(void);            /* 我喜欢的音乐 */
void api_request_liked_total(void);      /* 只取我喜欢歌单的曲目数（我的页面） */
void api_request_played(void);           /* 最近播放 */
void api_request_artist_songs(long long id, const char *name);
void api_request_comments(long long song_id, int offset);
/* offset=0 starts a session; more appends up to MAX_COMMENTS, no new allocation.
 * EV_COMMENTS_OK.index is 0 for initial load, 1 for append (preserve scroll). */
void api_request_comments_more(void);
int api_comments_has_more(void);
int api_comments_loading_more(void);
void api_request_user_detail(void);

const CardList *api_cards(void);
const CardList *api_home_cards(void);
const CommentList *api_comments(void);
int api_comment_total(void);
int api_comment_offset(void);
int api_comment_has_more(void);
int api_comment_page(void);
long long api_comment_song(void);
long long api_comment_target(void);
unsigned int api_comment_generation(void);
const char *api_comment_name(void);
const char *api_user_avatar(void);

/* 封面缩略图（歌单网格） */
int api_thumb_slot(const char *url);          /* 已就绪返回槽位，否则 -1 并排队 */
const unsigned short *api_thumb_px(int slot);
void api_request_thumbs(void);                /* 把排队的图抓回来 */
int api_avatar_slot(const char *url);         /* 评论头像 */
const unsigned short *api_avatar_px(int slot);

int api_busy(void);
void api_clear_events(void);     /* 丢掉还没处理的事件（MV 开始前用） */

/* 取事件：1 表示拿到一个 */
int api_poll(ApiEvent *ev);

/* 结果 */
const SongList *api_result(void);
const Lyric *api_lyric(void);
const unsigned short *api_art(int *w, int *h);
const unsigned short *api_backdrop(void);
const unsigned short *api_backdrop_dim(void);   /* 歌词页用的压暗版本 */
void api_clear_art(void);

#endif
