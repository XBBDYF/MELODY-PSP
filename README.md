# MELODY PSP

> 一个为 PSP 打造的第三方网易云音乐客户端：在线播放、歌词、歌单、评论、MV、可换肤，全部原生 C 实现。

MELODY 运行在 PSP-1000 / 2000 / 3000 / E1000（6.60 / 6.61 自制固件）上，使用 PSP 的 WLAN、
硬件 MP3 解码与 GPU 绘制，界面由 Lua 皮肤脚本驱动，因此换主题不需要重新编译程序。

本仓库是 **源码仓库**：包含主程序、启动器、皮肤运行时、示例皮肤、宿主机回归测试与构建脚本。
音乐与 MV 的可播放版本依赖第三方解码模块（见 [第三方说明](THIRD-PARTY-NOTICES.md)），
仓库不附带任何音乐内容。

- 作者：洗不白de衣服（GitHub [XBBDYF](https://github.com/XBBDYF)）
- 交流群（QQ）：**1103558445**
- 支持作者：[爱发电](https://afdian.com/a/hoi4-province-editor)
- 当前版本：**4.1.1**

![交流群](docs/assets/qq-group.png)

---

## 功能一览

| 模块 | 能力 |
|---|---|
| 发现 | 每日推荐、排行榜、推荐歌单、歌单广场、歌手榜 |
| 搜索 | 歌曲 / 专辑 / 歌手 / 电台，拼音联想，结果支持第三方音源补充 |
| 播放 | 在线流播（边下边播）、本地缓存优先、无版权自动走备用音源、断点续传、后台预取 |
| 歌词 | 逐行动画、翻译、单行/插页排版、摇杆与按键操作 |
| 歌单 | 收藏、我喜欢的音乐、最近播放、歌单详情、批量下载 |
| 评论 | 楼层展开、点赞、楼中楼 |
| MV | 在线 MV（软件解码模块 / 原生 AVC）、本地 VIDEO 目录扫描、下载与导出 |
| 皮肤 | Lua 5.4 主题 VM：metro / metro_day / xmb / paper 等 9 套示例皮肤，支持安装与在线更新 |
| 系统 | 在线更新（下载器 + 安装器）、开机自动联网、网速/电量显示、日志导出、隐私同意与公告 |

## 目录结构

```
MELODY-PSP/
├── src/                 主程序（C，PSP PRX）
├── tools/
│   ├── prxloader/       启动器与打包/签名脚本
│   ├── mvtest/          MV 解码冒烟程序（软件解码模块入口）
│   └── skin_templates/  metro 皮肤模板（生成器源文件）
├── skin_release/melody/skins/  可安装的示例皮肤（Lua）
├── third_party/         Lua 5.4.7、libjpeg-turbo 封装
├── tests/               宿主机回归测试（Python 驱动 gcc 编译片段）
├── docs/                架构、逻辑、构建、主题、版本记录
├── assets/              启动图等程序资源
├── Makefile             主程序构建
└── build.sh             单模块快速构建
```

## 快速开始

想直接玩：从 Releases 下载完整安装包，把 `PSP` 目录合并到记忆棒根目录即可（详见包的 `使用说明.txt`）。

想编译：见 [docs/BUILD.md](docs/BUILD.md)。三句话版本：

```bash
# 需要 pspdev（psp-gcc、PSPSDK）与 msys2 bash
./build.sh src/main.o src/net.o ...      # 单模块
make                                     # 主程序（melody.prx）
```

## 文档

| 文档 | 内容 |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 进程/线程模型、模块分层、内存与界面框架 |
| [docs/LOGIC.md](docs/LOGIC.md) | 播放、下载、MV、搜索、皮肤、更新等关键链路 |
| [docs/BUILD.md](docs/BUILD.md) | 构建环境、依赖、命令、宿主机测试 |
| [docs/THEMES.md](docs/THEMES.md) | 皮肤目录结构与 Lua API |
| [CHANGELOG.md](CHANGELOG.md) | 版本更新记录（4.1.1 / 4.1 / 4.0.1 …） |
| [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) | 第三方组件与许可证 |
| [CREDITS.md](CREDITS.md) | 致谢名单 |

## 许可证

本项目整体以 **GNU General Public License v3.0** 发布（见 [LICENSE](LICENSE)）。
原因：主程序静态链接了 WiliMini 的解码代码（GPL-3.0）与 FFmpeg（LGPL-2.1-or-later），
按 GPL 兼容性要求，合并后的可执行文件须整体按 GPL-3.0 分发。
`third_party/` 下的组件保留其各自的许可证，详见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

## 声明

- 本项目是非官方客户端，与网易公司无关，不提供、不托管任何音乐内容。
- 所有音频/视频均来自第三方公开接口，版权归原权利人所有；仅供个人学习与试听。
- 「网易云音乐」为网易公司商标。使用本程序产生的一切后果由使用者自行承担。

## 支持作者

如果 MELODY 帮你把吃灰的 PSP 又用了起来，可以到 [爱发电](https://afdian.com/a/hoi4-province-editor)
请作者喝一杯咖啡。打赏完全自愿，不影响任何功能，也不构成任何形式的服务承诺。

> https://afdian.com/a/hoi4-province-editor

