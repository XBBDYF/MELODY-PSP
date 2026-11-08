/*
 * MELODY PSP — 界面绘制
 *
 * 视觉参考网易云音乐网页版：深色顶栏 + 白色内容区 + 品牌红 #C20C0C。
 * 全部软件绘制（gfx.c），界面只读 g_app / g_list / audio_* 的当前状态。
 */

#ifndef MELODY_UI_H
#define MELODY_UI_H

#include "app.h"

/* 布局：480x272 的纵向分区 */
#define UI_NAV_H      28   /* 顶栏（黑） */
#define UI_HINT_H     13   /* 按键提示条（黑） */
#define UI_PLAYER_H   40   /* 底部迷你播放器（黑） */
#define UI_CONTENT_TOP UI_NAV_H

/* 列表 / 菜单行高 */
#define UI_ROW_H      28
#define UI_MENU_H     32
#define UI_COVER_SZ   80
#define UI_GRID_COLS  5

typedef struct{unsigned short panel,accent,text,muted,track;}UiVideoPalette;
void ui_video_palette(UiVideoPalette*out);
int ui_skin_icon(const char*name,int x,int y,int size,unsigned short color);
void ui_init(void);
void ui_free_skin_preview(void);
void ui_draw(void);
void ui_mv_connecting(const char *title, long long available, long long total);
void ui_toast(const char *msg);
void ui_theme_changed(void);   /* 切换主题后重建缓存的背景 */

void ui_document(const char *title,const char *text,int scroll,const char *footer);
#endif
