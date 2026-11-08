# GitHub 仓库记录

本页记录开源仓库本身的信息与操作方式：地址、分支、结构、提交历史、推送命令、
分发政策与 Issue 约定。**安装包不在这里发布**，见文末「分发政策」。

---

## 1. 仓库信息

| 项 | 值 |
|---|---|
| 地址 | https://github.com/XBBDYF/MELODY-PSP |
| 所有者 | [XBBDYF](https://github.com/XBBDYF)（洗不白de衣服） |
| 可见性 | Public |
| 默认分支 | `main` |
| 许可证 | GNU GPL v3.0（[LICENSE](../LICENSE)） |
| 描述 | PSP 原生网易云音乐客户端 · 原生 C + Lua 皮肤 · 开源 4.1.1 |
| Topics | `psp` `psp-homebrew` `homebrew` `netease` `lua` `c` `music-player` `cfw` |
| Issues | 开启（Wiki 关闭） |
| 本地路径 | `E:\MELODY-PSP-OPENSOURCE\MELODY-PSP` |

---

## 2. 仓库结构

```text
MELODY-PSP/
├── src/                      主程序源码（104 个 .c/.h/.S）
├── tools/
│   ├── prxloader/            启动器、打包与校验脚本
│   ├── mvtest/               MV 解码冒烟程序
│   ├── skin_templates/       metro / xmb 模板源文件
│   ├── make_update_splash.py 更新展示图生成
│   └── validate_skin_runtime.py
├── skin_release/melody/skins/  9 套 Lua 皮肤
├── third_party/              Lua 5.4.7、libjpeg-turbo（coverjpeg 封装）
├── tests/                    宿主机回归测试（125 个自包含用例）
│   └── integration/          需要额外产物的用例（默认不跑）
├── docs/                     架构 / 逻辑 / 构建 / 主题 / 截图 / 流程 / 本页
├── assets/                   启动图标与背景图
├── Makefile / build.sh / build.bat
├── README.md  LICENSE  CHANGELOG.md  CONTRIBUTING.md
├── CREDITS.md  THIRD-PARTY-NOTICES.md  .gitignore
```

统计（2026-11-08）：`git ls-files` 约 1,130 个文件，`.git` 约 16MB。

---

## 3. 提交历史

| 提交 | 时间 | 说明 |
|---|---|---|
| `d74303c` | 2026-11-08 15:22 | 首次开源提交：源码、皮肤运行时、测试与文档 |
| `f5bddfd` | 2026-11-08 15:23 | WiliMini 路径改为可覆盖变量；裁剪 libjpeg-turbo 文档；测试说明与批量脚本 |
| `e0b52d9` | 2026-11-08 15:49 | README 改为项目主页样式（徽章/新闻/架构图/运行逻辑图/截图/许可标注） |
| 本次 | 2026-11-08 | 截图页、工作流程、GitHub 记录；安装改为群内分发 |

> 仓库是在旧仓库（v3.1 仅可执行文件）删除后重建的，因此历史从 `d74303c` 开始，
> 不含任何旧提交。旧仓库删除前已镜像备份到 `E:\MELODY-PSP-OPENSOURCE\_old-repo-backup.git`。

---

## 4. 推送方式

```bash
cd E:\MELODY-PSP-OPENSOURCE\MELODY-PSP

git status                 # 确认工作区干净
git add -A
git -c user.name="XBBDYF" -c user.email="XBBDYF@users.noreply.github.com" \
    commit -m "修复：<现象>（根因 + 改动）"
git push
```

凭据走 Windows 凭据管理器（`git credential-manager`），不在仓库里存 token。

首次克隆别人的工作区时：

```bash
git clone https://github.com/XBBDYF/MELODY-PSP.git
```

---

## 5. 仓库里不放什么

`.gitignore` 明确排除，且**不要**为了「方便」强行提交：

| 排除项 | 原因 |
|---|---|
| `*.o` `*.prx` `*.PBP` `*.elf` `*.a` | 构建产物，随源码变化失效 |
| `releases/` `dist_*/` `backups/` `.tmp/` | 本机构建与备份，体积大且含私人物品 |
| `log.txt` `cache/` `*.part` `settings.txt` | 运行期数据，可能含账号 Cookie 与听歌记录 |
| `dns.txt` `update-preview*` | 本机网络缓存 |
| `__pycache__/` | 测试缓存 |

另：记忆棒备份、卡内截图原图、群友私聊截图都不入库。

---

## 6. 分发政策

**本仓库不提供任何安装包。**

- 安装包（完整安装 / 在线更新 / 一键替换）统一发在 **QQ 群 1103558445** 的群文件；
- GitHub 上不建带二进制的 Release，README 的安装章节直接指向群；
- 理由：二进制与源码各自只有一个来源，避免「群里传的旧包」与「仓库新代码」混在一起；
- 如果以后需要给 Release，也必须同时满足 GPL 的源码提供义务（本仓库即为对应源码）。

---

## 7. Issue 约定

开 Issue 时请带上：

1. **现象**：哪个页面、哪首歌、卡了多久、是否必现；
2. **环境**：PSP 型号（1000/2000/3000/Go）、固件版本、自制系统（如 6.61 ARK）、当前主题；
3. **日志**：`ms0:/PSP/GAME/MELODY/log.txt`（设置 → 关于 可导出）；
4. **对比**：上一版是否正常；PPSSPP 是否复现。

日志里的关键字能大幅缩短排查时间：

```text
dns done ms=... result=-1        解析超时
dns http ok <host>               走了 HTTP 备用解析
audio error, retry N: <原因>     解码或取流失败
dl stream: begin failed ...      下载流没起来
skin io: ... rc=0x...            皮肤写卡失败
```

---

## 8. 第三方与许可标注位置

| 文件 | 作用 |
|---|---|
| [README 致谢表](../README.md#致谢) | 每个依赖的用途 + 许可证（GPL-3.0 / LGPL-2.1 / MIT / BSD-3 / OFL-1.1 …） |
| [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) | 版本、使用范围、许可证原文说明、GPL 影响分析 |
| [CREDITS.md](../CREDITS.md) | 人与项目，含参考项目（PPSSPP / PMPlayer / NeteaseCloudMusicApi / MElib） |
| 设置 → 关于（程序内） | 精简版第三方清单（`src/credits.h`） |

修改依赖时，这三处 + 程序内 `credits.h` 都要同步更新，否则会出现「仓库说 MIT、
程序里写 GPL」这种自相矛盾。
