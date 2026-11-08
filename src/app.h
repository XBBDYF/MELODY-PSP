/*
 * MELODY PSP — 公共数据模型
 */

#ifndef MELODY_APP_H
#define MELODY_APP_H

#define APP_DIR   "./"
#define APP_TITLE "MELODY"
#define APP_VER   "4.1.1"
#define MELODY_SETTINGS_COUNT 25

#define MAX_SONGS 600          /* 列表上限（歌单按页加载，600 首约占 260KB 静态内存） */
#define MAX_CARDS  24
#define MAX_COMMENTS 40
#define GRID_PER     8
#define MAX_LYRIC 90

/* 列表来源 */
enum {
    LK_CHART = 1,
    LK_SEARCH,
    LK_RECENT,
    LK_FAV,
    LK_CACHE,
    LK_DAILY,      /* 每日推荐 */
    LK_LIKED,      /* 我喜欢的音乐 */
    LK_PLAYED,     /* 最近播放 */
    LK_PLAYLIST,   /* 歌单详情 */
    LK_ARTIST      /* 歌手热门 */
};

/* 封面卡片（歌单 / 榜单 / 歌手） */
typedef struct {
    long long id;
    char name[64];
    char cover[520];
    char sub[48];        /* 创建者 / 更新频率 / 播放量 */
    long long plays;
    int tracks;
    int kind;            /* 0 歌单 1 榜单 2 歌手 */
    int fld;
} Card;

typedef struct {
    Card items[MAX_CARDS];
    int count;
    char title[48];
    int kind;            /* 0 推荐歌单 1 我的歌单 2 排行榜 3 歌单广场 4 歌手榜 */
} CardList;

/* 一条评论 */
typedef struct {
    long long id;
    int reply_count;
    char user[32];
    char text[1024];
    char ip[24];
    char avatar[168];
    int liked;
    long long time;
    int hot;
} Comment;

typedef struct {
    Comment items[MAX_COMMENTS];
    int count;
    int total;
} CommentList;

typedef struct {
    long long id;
    char name[72];
    char artist[72];
    char album[44];
    char art[208];      /* 网易云封面 URL（http） */
    int dur_ms;
    int fee;
    int pop;            /* 热度（搜索结果里有，越大越热门） */
    int hot;            /* 排序后标出来的「热门」 */
    int playable;       /* 0 未知 / 1 可播 / 2 不可播 */
    int mv;             /* 网易云 MV id，0 = 没有 MV */
} Song;

typedef struct {
    Song songs[MAX_SONGS];
    int count;
    char title[48];
    long long chart_id;
    int kind;
} SongList;

typedef struct {
    int ms[MAX_LYRIC];
    char text[MAX_LYRIC][96];
    char translation[MAX_LYRIC][144];
    int translated_count;
    int count;
} Lyric;

typedef struct {
    long long id;
    char name[24];
    char tag[20];
    unsigned short c1;   /* 卡片渐变色 */
    unsigned short c2;
    int icon;            /* 0 火 1 新 2 升 3 搜索 */
} Chart;

/* 网络任务事件 */
enum {
    EV_NONE = 0,
    EV_SKIN_INSTALLED,
    EV_SEARCH_ENTITIES,
    EV_NET_OK,
    EV_NET_FAIL,
    EV_LIST_OK,
    EV_LIST_FAIL,
    EV_CHECK_DONE,
    EV_PLAY_OK,
    EV_PLAY_FAIL,
    EV_ART_OK,
    EV_LYRIC_OK,
    EV_CACHE_DIRTY,
    EV_DL_STARTED,
    EV_DL_FAIL,
    EV_INFO,         /* 一条给用户看的提示（导出结果等） */
    EV_LIST_MORE,    /* 搜索结果下一页 */
    EV_SUGGEST_OK,   /* 搜索联想候选到了 */
    EV_QR_NEW,       /* 二维码取到了 */
    EV_QR_STATE,     /* 扫码状态变了（801/802/803/800） */
    EV_LOGIN_OK,     /* 登录成功，账号信息也就位了 */
    EV_CARDS_OK,     /* 歌单/榜单卡片到了 */
    EV_DAILY_OK,     /* 每日推荐歌曲到了 */
    EV_SONGS_OK,     /* 通用歌曲列表到了（我喜欢/最近播放/歌手热门） */
    EV_COMMENTS_OK,  /* 评论到了 */
    EV_THUMB_OK,     /* 一张封面缩略图就绪 */
    EV_LIKE_OK       /* 喜欢/收藏状态变了 */
};

typedef struct {
    int kind;
    int index;
    long long subject_id;       /* song/comment subject; 0 for global events */
    unsigned int account_generation;
    unsigned int request_generation;
    char msg[72];
} ApiEvent;

/* 屏幕 */
enum {
    SC_BOOT = 0,
    SC_WIFI,
    SC_HOME,
    SC_LIST,
    SC_PLAYER,
    SC_LYRICS,
    SC_SEARCH,
    SC_DOWNLOAD,
    SC_CACHE,
    SC_SETTINGS,
    SC_HELP,
    SC_LOGIN,
    SC_GRID,
    SC_SQUARE,
    SC_COMMENTS,
    SC_MINE,
    SC_MV,
    SC_ALT,
    SC_SKINS,
    SC_COUNT
};

#define MAX_PROFILES 8

/* 首页（发现）的条目编号：0 每日推荐 / 1..3 榜单 / 4..13 推荐歌单 / 14..18 快捷入口 */
#define HOME_ITEM_HERO   0
#define HOME_ITEM_CHART  1
#define HOME_ITEM_COVER  4
#define HOME_ITEM_QUICK  14
#define HOME_ITEM_COUNT  19
#define APP_CHIP_MAX      6    /* 搜索页：推荐搜索最多几个 */
#define APP_CHIP_LEN      24
#define HOME_COVERS      10
#define HOME_QUICKS      5

typedef struct {
    int id;
    char name[64];
    char ssid[64];
    int secure;
} Profile;

typedef struct {
    int screen;
    float enter;          /* 当前屏幕进场动画 0..1 */
    float time;           /* 全局时间（秒） */

    /* ---- 外观 / 下载 ---- */
    int theme;            /* 0 = 浅色（网页版） 1 = 深色 */
    int dl_parts;         /* 下载并发连接数 2..6 */

    /* ---- 首页（发现） ---- */
    int home_sel;
    float home_sel_f;
    float home_scroll;
    int home_row;
    int cards_stay;       /* EV_CARDS_OK 到达时不切屏（首页预取推荐歌单） */
    int cards_open;       /* 用户按了「更多/推荐歌单」：卡片到达后才进网格 */
    int cards_ready;      /* 首页推荐歌单是否加载过 */
    int tab;              /* 顶栏高亮：0 发现 / 1 我的 / 2 搜索 */
    float mine_scroll;
    int cat_focus;        /* 歌单广场：0 分类行 / 1 封面网格 */
    int confirm_clear;    /* 二次确认：1 清缓存 / 2 清下载队列 */

    /* 在线更新弹窗（启动静默检查发现新版本时显示） */
    int upd_dlg;          /* 0 关 / 1 显示 */
    int upd_sel;          /* 0 = 稍后 / 1 = 立即更新 */
    int upd_checked;      /* 本次启动已经检查过 */
    int upd_dismissed;    /* 按过"稍后"，本次启动不再弹 */
    int upd_scroll;       /* 更新日志滚动行 */

    int list_sel;
    int download_select_mode;
    int download_selected_count;
    float list_scroll;

    /* 搜索页 */
    int kb_row, kb_col;   /* 自制键盘光标 */
    int kb_edit;          /* 1 = 键盘输入中，0 = 看结果 */
    char query[48];
    int query_len;
    int search_state;
    int search_kind;     /* 0 空 / 1 搜索中 / 2 有结果 / 3 失败 */
    char hist[6][40];
    int hist_n;
    /* 搜索页：推荐/历史 chip 与输入法候选（ui.c 只读） */
    char chips[APP_CHIP_MAX][APP_CHIP_LEN];
    int chips_n;
    int chip_sel;          /* 当前选中的 chip（0 起） */
    int chip_row;          /* 0 = 第一行(历史/候选)，1 = 推荐行 */
    int ime_sel;           /* 候选序号（0 起） */
    int hist_sel;          /* 历史列选中项（0 起） */
    int ime_off;           /* 候选横向滚动窗口起点 */
    char assoc[APP_CHIP_MAX][APP_CHIP_LEN];  /* 上屏后的本地词库联想 */
    int assoc_n;

    /* 搜索联想 */
    int sug_focus;       /* 1 = 光标在右侧联想列表里 */
    int sug_sel;
    int sug_state;       /* 0 空 / 1 请求中 / 2 有候选 */
    float sug_t;         /* 防抖计时 */
    char sug_last[48];   /* 上次发出去的词，避免重复请求 */

    /* 分页 */
    int list_total;      /* 服务端命中总数 */
    int list_more;       /* 1 = 正在加载下一页 */

    /* 封面网格 */
    int grid_sel;
    float grid_scroll;
    int grid_page;       /* 第几屏（每屏 6 个） */
    int grid_kind;
    char grid_cat[24];   /* 歌单广场分类 */
    int cat_sel;

    /* 评论页 */
    int cm_sel;
    float cm_scroll;
    int cm_more;         /* 正在加载更多评论 */

    /* 我的 */
    int mine_sel;
    int user_level;
    int user_listen;

    int dl_sel;
int dl_tab;          /* 下载页：0 歌曲 1 MV */
    int cache_sel;
    int cache_tab;     /* 0 = 缓存  1 = 本地音乐 */
    int local_sel;
    float list_fade;

    int settings_sel;
    int alt_sel;       /* 备用音源页光标 */
    int show_mem;      /* 右下角内存显示 */
    int help_page;
    int skin_sel;
    float skin_scroll;
    int quality;          /* 0 标准128k / 1 较高192k / 2 极高320k */

    int lyrics_open;
    int lyric_translation;
    float lyric_scroll;

    int wifi_state;
    int online;
    char wifi_name[40];

    /* 连接页 */
    int wlan_on;
    int profile_count;
    int profile_sel;
    Profile profiles[MAX_PROFILES];
    int connecting;       /* 0 空闲 / 1 连接中 / 2 成功 / 3 失败 */
    char connect_msg[64];
    float connect_t;
    char net_msg[64];

    char toast[96];
    float toast_t;

    int playing_index;
    int play_from;     /* 从哪个界面进入播放页（× 返回用） */
    int play_from_tab; /* 缓存页的标签页 */
    int has_now;
    Song now;

    int paused;
    float art_zoom;
    float eq_phase;
    float marquee;

    int play_all_next;    /* 列表回来后直接开播 */
    int play_mode;        /* 0 顺序 1 随机 2 单曲循环 */
    int back_stack[6];
    int back_n;
    int list_from;
    int comment_from;
    int login_from;
    int grid_cols;        /* 网格每行几个 */
    int liked_page;
    int is_liked;
    int menu_open;        /* 播放页功能弹窗 */
    int menu_sel;
    int busy;             /* 后台任务进行中 */
    int busy_x;           /* 转圈画在标题右边（由顶栏算出来） */
    float spinner;
} AppState;

extern AppState g_app;
extern SongList g_list;
extern int g_volume;

int app_download_selected(long long id);
#endif

