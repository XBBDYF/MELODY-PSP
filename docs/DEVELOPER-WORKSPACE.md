# MELODY PSP 开发工作区

MELODY 是 PSP 原生音乐播放器，当前开发构建标记 4.1.1。公开源码与完整介绍：https://github.com/XBBDYF/MELODY-PSP 。安装/更新包通过 QQ 群 1103558445 分发，不在 GitHub Releases 发布应用二进制。

## 确认／取消按键

设置中的“确认键”提供：

- 跟随系统：读取 PSP 系统的 ○／× 确认偏好；读取失败回退 ○确认、×取消。
- ○确认、×取消。
- ×确认、○取消。

手动选择保存在程序目录 `buttons.cfg`，重启保留。切换对原生界面、弹窗、极致省电播放器和内置 Lua 皮肤使用同一逻辑。系统联网对话框继续遵循系统自己的按键偏好，不被应用反向映射。

原生提示与官方主题按键说明随选择切换；歌曲标题、搜索内容与歌词不参与符号替换。Lua 主题可用 `ui.button_hint(text)` 映射提示，用模型 `confirm_key`、`cancel_key`、`button_cross_confirm` 获取物理按键说明。

## 极致列表快移与单曲循环修复

极致省电列表：↑↓逐首、←→每次快移8首，□切换本地/完整缓存。START播放器选项可在列表循环与单曲循环间切换。
普通与极致模式单曲循环都需在播完后重新建立解码线程；不能仅seek(0)，因为EOF时旧线程可能已经退出。完整本地/缓存直接从原路径重开，未完整下载的在线流保留原请求路径处理，不把半截文件当完整本地媒体。

## 阅读入口

- 接手维护：[AI-READ-FIRST.md](AI-READ-FIRST.md)
- 最近修改与证据：[WORKLOG.md](WORKLOG.md)
- 运行架构：[PROJECT-RUNTIME.md](PROJECT-RUNTIME.md)
- 给人类与AI的主题制作包：[THEME-KIT/README.md](THEME-KIT/README.md)
- 极致省电/歌词封面/时钟：[docs/POWER-PROFILES.md](docs/POWER-PROFILES.md)
- 可独立更新的输入法词库：[docs/IME-PACK.md](docs/IME-PACK.md)

## 验证与构建

```text
python -B tests/test_input_buttons.py
```

PSP 对象通过 `build.sh` 编译，最终实际启动的主 PRX 由 `tools/prxloader/build.sh` 链接。主机测试、模拟器验证和物理 PSP 测试分别记录，不把任一层结果替代另一层。

## 输入法响应与逐段输入

扩展词库在加载校验后放入PSP独立内存快照，按键不再逐记录读卡；进入MV/极致省电释放这块内存。内存不足回内置词库，不回落到卡顿的逐字磁盘查词。
多音节如chuyin、tiaotiao可在整词之外选择当前首音节单字；选“初”后保留yin，选“跳”后保留第二个tiao。未开省电时不显示常规模式标记。

## ECO颜色与MV内存

ECO颜色选项同时更新Lua自绘页、原生回退页和状态栏的完整调色板。未开启省电时不显示常规模式标记。
MV内存提示以日志的总可用/最大连续块/所需预算定位；新增词库快照与缩略图缓存须在MV前释放，退出后恢复，不通过盲降门槛掩盖内存不足。

MV实机日志此前差320KiB总内存；已将723KiB首页缩略图/头像BSS改为可回收高分区缓存。保留原门槛，实际MV是否连续起播仍需设备复测。
