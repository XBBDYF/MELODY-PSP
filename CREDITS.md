# 致谢

MELODY 能跑起来，靠的不只是这个仓库里的代码。这里按「帮了什么忙」列出所有该感谢的人与项目。

## 作者

- **洗不白de衣服（XBBDYF）** — 程序、界面、皮肤、打包与全部真机调试
  - GitHub：https://github.com/XBBDYF
  - B 站：https://space.bilibili.com/535975174
  - 官网：https://xbbdyf.github.io/retro/index.html

## 交流与反馈

- **PSP 网易云交流群（QQ 群号：1103558445）** — 版本测试、问题复现、真机验证；
  很多「某首歌卡住」「某张卡顿」的问题都是群友先发现、再一起定位的。

## 代码与库

| 项目 | 帮了什么 |
| 项目 | 帮了什么 | 许可证 |
|---|---|---|
| **[WiliMini](https://github.com/wilimini/WiliMini)** | 软件视频解码的实现与经验：`pspvideo` 接口、FFmpeg 在 PSP 上的裁剪与性能处理。没有它就没有 MV 播放 | **GPL-3.0** |
| **[FFmpeg](https://ffmpeg.org/)** | 容器解析与音视频解码的底座 | **LGPL-2.1-or-later** |
| **[pspdev / PSPSDK](https://github.com/pspdev/pspdev)** | 工具链、`sce*` 接口、启动器打包；PSP 自制程序的地基 | **BSD-3-Clause** |
| **[Lua](https://www.lua.org/)** | 皮肤 VM：让换主题不用重新编译 | **MIT** |
| **[libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)** | 封面解码（经 `coverjpeg_psp` 封装） | **BSD-3-Clause / IJG / zlib** |
| **[zlib](https://zlib.net/)** | gzip 解压与更新包压缩 | **zlib License** |
| **mcidclan（m-c/d）** — [MElib / PSP ME MIST](https://github.com/mcidclan) | Media Engine 硬解与 Custom Core 的研究资料 | **MIT** |
| **[PPSSPP](https://github.com/hrydgard/ppsspp)** | 接口行为对照与快速迭代验证；大量疑点先在模拟器上排除 | 参考（GPL-2.0-or-later） |
| **[PMPlayer Advance](https://github.com/DavisDev/pmplayer-advance)** | AVC 播放流程参考 | 参考（GPL-2.0） |
| **[NeteaseCloudMusicApi](https://github.com/Binaryify/NeteaseCloudMusicApi)** | 接口字段与调用方式资料（本程序直接请求官方接口，未内置该服务） | 参考（MIT） |
| **思源黑体 / WenQuanYi Micro Hei** | 点阵字库的来源字体 | **SIL OFL 1.1 / Apache-2.0** |

## 特别感谢

- 所有公开 PSP 硬件资料、逆向笔记与论坛帖子的作者——`sceMp3` 的边界条件、
  ME 的通信方式、FAT 驱动的怪脾气，都是踩着这些资料试出来的。
- 所有提交日志、复现步骤与截图的朋友。这个项目里很多「玄学问题」的根因
  （DNS 解析超时、`O_TRUNC` 竞争、写卡瞬时失败）都是靠真机日志一行行对出来的。
- 屏幕前的你：愿意在 2026 年还拿出 PSP 试一个第三方客户端。

## 一段说明

本项目以 GPL-3.0 开源，是因为链接了 GPL 的解码实现。如果你只是想自己改着玩，
不用管这些；如果你要分发修改版，请把源码一起公开，让下一个改 PSP 的人少走点弯路。
