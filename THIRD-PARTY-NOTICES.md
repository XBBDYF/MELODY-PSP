# 第三方组件与许可证

本仓库整体以 **GPL-3.0** 发布（见 [LICENSE](LICENSE)），因为主程序静态链接了
WiliMini 的解码代码（GPL-3.0）与 FFmpeg（LGPL-2.1-or-later）。
下列组件保留各自的许可证与版权声明，**不以 GPL-3.0 重新授权**。

---

## 一、仓库内直接包含

### Lua 5.4.7

- 位置：`third_party/lua-5.4.7/`
- 许可证：MIT（见该目录 `LICENSE` / `COPYRIGHT`）
- 用途：皮肤与主题脚本的虚拟机
- Copyright © 1994–2024 Lua.org, PUC-Rio

### libjpeg-turbo 3.1.3（经 `coverjpeg_psp` 封装，仅解码）

- 位置：`third_party/coverjpeg_psp/`（含 `libjpeg-turbo-3.1.3/` 源码、`private-src/` 封装、`decode-only.patch`）
- 许可证：BSD-3-Clause（libjpeg-turbo）+ IJG + zlib（三选一，实际按 BSD-3-Clause 使用）
- 用途：封面 JPEG 解码（PSP 硬件 `sceJpeg` 的补充路径）
- Copyright © 1991–2020 Thomas G. Lane, Guido Vollbeding；© 2009–2024 libjpeg-turbo contributors

### pinyin-data（单字读音表）

- 位置：`tools/ime/pinyin-data.txt`
- 许可证：**MIT**
- 来源：https://github.com/mozillazg/pinyin-data（基于 Unicode Unihan 的读音整理）
- 用途：`tools/build_ime_dict.py` 生成搜索结果里的单字候选表（k_ime_chars[]），不进入绘制/解码路径

### 点阵字库

- 位置：`src/font12.bin` `src/font16.bin` `src/font22.bin` `src/font32.bin`
- 来源字体：思源黑体（SIL Open Font License 1.1）、WenQuanYi Micro Hei（Apache-2.0 / GPL-2.0 双许可，此处按 Apache-2.0 使用）
- 用途：以 `sfont` 工具生成的点阵数据，按 SIL OFL 1.1 保留字体名称不与原字体混淆

### 启动图标与图片

- 位置：`assets/`
- 由项目作者制作或从公有领域素材修改

---

## 二、构建/运行依赖（不随仓库分发源码）

### WiliMini 解码参考实现

- 许可证：**GPL-3.0**
- Copyright © 2026 WiliMini contributors
- https://github.com/wilimini/WiliMini
- 用途：MV 软件解码（`pspvideo` 头文件与 `libpspvideo_cpu.a`），
  以及 `PSP ME MIST / Custom Core` 相关研究
- 影响：主程序链接其代码，因此本项目整体按 GPL-3.0 发布

### FFmpeg 6.1.2

- 许可证：**LGPL-2.1-or-later**
- Copyright © 2000–2024 the FFmpeg developers
- https://ffmpeg.org/
- 用途：MV 的容器/视频/音频解析（静态链接进 `melody.prx`）

> 本程序使用 FFmpeg 项目的库（LGPLv2.1）。
> This software uses libraries from the FFmpeg project under the LGPLv2.1.

### PSP ME MIST / Custom Core

- 许可证：MIT
- Copyright © 2025–2026 m-c/d（mcidclan）
- https://github.com/mcidclan
- 用途：Media Engine 硬解流程研究

### PSPSDK / pspdev

- 许可证：BSD-3-Clause（PSPSDK 部分）
- Copyright © 2005 adresd, M. R. Brown, J. Forshaw, John Kelley, Jesper Svennevid
- https://github.com/pspdev/pspdev
- 用途：工具链、`sce*` 头文件与 stub 库、启动器打包工具

### newlib

- 许可证：BSD 风格（见 newlib 的 `COPYING.NEWLIB`）
- 用途：C 运行库

### GCC libgcc

- 许可证：GPL-3.0 + GCC Runtime Library Exception
- 用途：编译器运行时

### zlib

- 许可证：zlib License
- 用途：gzip 解压、在线更新包的压缩模块

---

## 三、参考资料（未直接包含代码）

| 项目 | 用途 | 链接 |
|---|---|---|
| PPSSPP | PSP API 行为、模拟器验证 | https://github.com/hrydgard/ppsspp |
| PMPlayer Advance | AVC 解码流程参考 | https://github.com/DavisDev/pmplayer-advance |
| NeteaseCloudMusicApi | 接口字段与调用方式资料（本程序直接请求官方接口，未内置该服务） | https://github.com/Binaryify/NeteaseCloudMusicApi |
| MElib | ME 通信研究 | https://github.com/IridescentRose/MElib |

---

## 四、免责声明

- 本项目不是网易公司的产品，与网易公司无关；不提供、不托管任何音乐内容。
- 「网易云音乐」及相关标识归网易公司所有。
- 第三方接口随时可能变更或关闭，本项目不保证任何接口长期可用。
- 使用者应自行确认所在地区关于网络服务与版权内容的法律要求。

如你是权利人并认为本仓库某部分侵犯了你的权利，请通过仓库 Issue 联系，我们会移除相关内容。

## 输入法词频数据（2026-11-11）

jieba v0.42.1：MIT / Copyright (c) 2013 Sun Junyi。原始词典与完整许可见
`tools/ime/jieba/dict.txt`、`tools/ime/jieba/LICENSE`，来源说明见同目录`SOURCE.md`。
生成器仅采用最多8192个高频且字体覆盖的词，与人工表去重；没有引入jieba执行库。
pinyin-data：MIT / Copyright (c) 2016 mozillazg，完整许可见`tools/ime/pinyin-data.LICENSE`。
