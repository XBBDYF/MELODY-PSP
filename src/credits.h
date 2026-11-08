/*
 * MELODY PSP — 「关于」页文案（UTF-8，纯数据，无依赖）
 * ------------------------------------------------------------------
 * 用法：ui.c 里 #include "credits.h"，逐行 gfx_text(FS_SM, ...)。
 *
 * 约定（由 screen_help 的绘制片段解释）：
 *   · 以 "## " 开头的行  = 小节标题（绘制时去掉前缀，用 C_ACC 强调色）
 *   · 空字符串 ""        = 段落间隔（占一行高度，不绘制）
 *   · 其余行             = 正文（C_SUB）
 *
 * 宽度预算：12px 字库、x=24、右侧留 24 ⇒ 可用 432px。
 *   全角 12px/字 ⇒ 每行最多 36 个全角；ASCII 约 6px/字。
 *   下面每行都控制在 ~34 个全角以内（最长一行约 300px）。
 *
 * 字符集：FS_SM 是 GB2312 点阵，只用常用字；不要用 ©、•、★ 等
 *   GB2312 之外的符号（「」·—… 是 GB2312 内的，可用）。
 *
 * 项目计划开源；最终许可证随源码发布，不宣称仓库已公开。
 * 已有来源记录见 docs/MV-QUALITY-WORK.md、
 * docs/HARDWARE-DECODE-RESEARCH-20261102.md；第三方保留各自许可证。
 */

#ifndef MELODY_CREDITS_H
#define MELODY_CREDITS_H

static const char *const k_credits[] = {
    "## MELODY PSP",
    "第三方网易云音乐客户端（非官方）",
    "仅供个人学习与试听",
    "",

    "## 作者与联系",
    "作者：洗不白de衣服",
    "GitHub：XBBDYF",
    "github.com/XBBDYF",
    "B站：535975174",
    "space.bilibili.com/535975174",
    "官网：xbbdyf.github.io/retro",
    "",
    "## 开源计划与许可证",
    "项目计划开源，源码仓库尚待公布。",
    "计划开源，最终许可证随源码发布；",
    "第三方组件各自遵循其许可证。",
    "",

    "## Lua皮肤脚本引擎",
    "Lua 5.4.7 / MIT License",
    "https://www.lua.org/",
    "Copyright (C) 1994-2024 Lua.org, PUC-Rio. Permission is hereby",
    "granted, free of charge, to any person obtaining a copy of this",
    "software and associated documentation files (the \"Software\"), to",
    "deal in the Software without restriction, including without",
    "limitation the rights to use, copy, modify, merge, publish,",
    "distribute, sublicense, and/or sell copies of the Software, and to",
    "permit persons to whom the Software is furnished to do so, subject",
    "to the following conditions: The above copyright notice and this",
    "permission notice shall be included in all copies or substantial",
    "portions of the Software. THE SOFTWARE IS PROVIDED \"AS IS\",",
    "WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT",
    "NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A",
    "PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE",
    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR",
    "OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR",
    "OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE",
    "OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.",
    "",

    "## 参考代码与开源仓库",
    "WiliMini / 软件视频播放参考",
    "https://github.com/wilimini/WiliMini",
    "PPSSPP / PSP接口与模拟器参考",
    "https://github.com/hrydgard/ppsspp",
    "FFmpeg / 软件解码器依赖",
    "https://github.com/FFmpeg/FFmpeg",
    "PMPlayer Advance / AVC解码流程参考",
    "https://github.com/DavisDev/pmplayer-advance",
    "ME通信 / Custom Core研究参考",
    "github.com/mcidclan/",
    "psp-media-engine-cracking-the-unknown",
    "https://github.com/IridescentRose/MElib",
    "网易云API接口资料参考（非内置服务）",
    "github.com/Binaryify/NeteaseCloudMusicApi",
    "本应用直接请求相关服务，未内置该服务器。",
    "",
    "## 第三方组件",
    "· WiliMini 解码器（GPL-3.0）",
    "  Copyright (C) 2026 WiliMini contributors",
    "  github.com/wilimini/WiliMini",
    "· FFmpeg 6.1.2（LGPL-2.1-or-later）",
    "  Copyright (c) 2000-2024 the FFmpeg developers",
    "  ffmpeg.org",
    "· PSP ME MIST / Custom Core（MIT）",
    "  Copyright (c) 2025-2026 m-c/d",
    "  github.com/mcidclan",
    "· PSPSDK（BSD-3-Clause）",
    "  (C) 2005 adresd, M. R. Brown, J. Forshaw,",
    "  John Kelley, Jesper Svennevid",
    "· newlib（BSD 风格，见 COPYING.NEWLIB）",
    "· GCC libgcc（GPL-3.0 + Runtime Exception）",
    "",

    "## 字体",
    "点阵字库由思源黑体 / WQY Micro Hei 生成",
    "（OFL-1.1 / Apache-2.0）",
    "",

    "## 声明",
    "本程序使用 FFmpeg 项目的库（LGPLv2.1）。",
    "This software uses libraries from the FFmpeg",
    "project under the LGPLv2.1.",
    "本程序通过网易云音乐公开接口获取信息，",
    "仅供个人学习与试听。",
    "「网易云音乐」版权归网易公司所有，",
    "本程序与网易公司无关，不提供任何音乐内容。",
    "使用本程序产生的后果由使用者自行承担。",
    "",

    "## 感谢",
    "WiliMini / xeonliu",
    "mcidclan（m-c/d）",
    "FFmpeg 项目及全体贡献者",
    "pspdev / PSPSDK 社区",
    "PPSSPP 项目",
    "网易云音乐",
    "以及所有公开 PSP 硬件资料的开发者",
    "",
    "## 测试反馈与支持",
    "所有参与真机测试、反馈的朋友",
    "支持名单待作者补充",
};

#define K_CREDITS_COUNT ((int)(sizeof(k_credits) / sizeof(k_credits[0])))

#endif /* MELODY_CREDITS_H */
