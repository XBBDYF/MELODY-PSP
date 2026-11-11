# 省电、背光超时与主题接入

核对：2026-11-11，开发构建仍显示 4.1.1。功能由 C 层执行，主题只提交用户操作。

## 使用

设置末尾新增三项，保留原有 0..24 设置 ID：

| 设置 | 可选值 | 默认行为 |
|---|---|---|
| 省电模式（ID 25） | 关 / 开 | 关；开时自动时钟目标为 222MHz，静置 2 秒后 UI 最高约 30 帧 |
| 自动息屏（ID 26） | 跟随系统 / 30 / 60 / 120 / 300 秒 / 常亮 | 跟随系统 |
| SoC 时钟（ID 27） | 自动 / 222 / 266 / 333 MHz | 自动；未开省电时维持应用正常性能档 |

配置保存在 EBOOT 同目录的 `power.cfg`。写入临时文件后备份旧文件并换名；失败回滚，启动可从 `power.cfg.old` 恢复。
关闭省电模式会取消空闲降帧；若手动选了 222MHz，仍保留该手动选择。要恢复正常策略，设为“关 + 自动 + 跟随系统”。

ECO 主题放在 `skin_release/melody/skins/eco/`。在主题设置中可调整这三项；首页 □ 可切省电，SELECT 进全局设置。
应用主题本身不会擅自更改用户电源配置。ECO 仅覆盖首页和播放页，搜索等功能回落原生完整界面。

## 策略与边界

- 操作后 2 秒内保持原来的交互帧率；省电开启且无操作时，跳过部分绘制与帧缓冲复制，等待 VBlank，继续处理事件、音频和下载。
- 这不是“按脏区域绘制”或静止零刷新：时钟、进度和状态仍会更新。
- MV 进入前强制 333MHz，连接/播放期间保持性能档；结束后按用户配置恢复。
- API 繁忙或音频缓冲时，低频档临时提升到至少 266MHz，优先保证起播/网络工作。
- 每轮读回硬件频率，防止固件休眠恢复或其他模块改变时钟后缓存失效；允许 PLL 整数读数差 1MHz，避免反复调频。
- 自定义超时到达后停止画面输出和 UI 重绘，音频/下载线程继续。第一次方向/按键只唤醒，持续按住也不触发误操作，释放后才恢复正常输入。HOLD 开关不算持续活动。
- BOOT、WiFi 配置页及 MV 保持显示，不触发自定义黑屏。常亮只阻止显示超时，不锁电源开关。
- 正常退出和 HOME 强制退出回调都恢复启动前时钟与背光超时。物理按键、固件接管和异常断电的最终硬件行为由系统决定。

## 真正背光超时与黑屏的区别

已核实 PSPSDK 的用户模式接口：

```c
int sceImposeGetBacklightOffTime(void);
int sceImposeSetBacklightOffTime(int seconds);
```

两个导入在 SDK 默认 `libpspuser.a` 内，无需内核插件。支持值按 SDK 文档采用 30 秒起。
启动保存系统原值；选自定义值时设置固件背光计时；退出恢复原值。常亮通过 `scePowerTick(PSP_POWER_TICK_DISPLAY)` 实现，不猜测 `Set(0)` 的语义。

接口返回失败时，只执行停止画面输出和暂停绘制，背光仍交由系统原设置。设置页会显示“仅画面”，Lua 模型 `power_backlight_timer=0`。
`SetFrameBuf(NULL)` 本身不等于关闭背光；PPSSPP 也不模拟真实背光。背光实际关闭、电池续航、多固件兼容尚需 PSP 实机测试。

来源：
- [PSPSDK 用户接口](https://github.com/pspdev/pspsdk/blob/master/src/user/pspimpose.h)
- [PSPSDK 导入表](https://github.com/pspdev/pspsdk/blob/master/src/user/sceImpose.S)
- [PPSSPP sceImpose 实现](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceImpose.cpp)

## 主题作者 API

明确的用户操作中派发，不要每帧调用：

```lua
app.dispatch("power_eco", "1")       -- 0 / 1
app.dispatch("power_timeout", "60")  -- 0=跟随系统，-1=常亮，30/60/120/300
app.dispatch("power_clock", "222")   -- 0=自动，222/266/333
```

只读模型：`power_eco`、`power_timeout`、`power_clock`（配置目标）、`power_effective_clock`（实际MHz）、`power_blanked`、`power_backlight_timer`。

可在 `preferences` 中声明保留 ID（值是索引，不是秒/MHz）：

- `system_eco`：toggle，0/1。
- `system_timeout`：choice，索引顺序 跟随系统/30秒/60秒/120秒/300秒/常亮。
- `system_clock`：choice，索引顺序 自动/222/266/333。

主题设置弹窗打开时显示真实全局值；只有用户调整选项才调用硬件策略并保存。

## 验证与功耗声明

- `tests/test_power_policy.py`：超时、唤醒吞键、MV保护、帧率门槛、毫秒回绕。
- `tests/test_power_runtime.py`：真实适配代码配合假硬件，验证配置往返、背光设置失败回落、退出恢复、硬件频率改变后的重新应用。
- 模拟器证据：`power-wake-smoke-20261111.log`记录30秒背光参数返回成功，39.8秒停止framebuffer，44.9秒`display wake`；ECO加载正常。实际物理背光和续航仍未测。
- 当前未自动断 WiFi，避免误断流媒体或下载；也未自动调物理亮度。
- 删除先前未经实机测量的“省电20–35%”等估算，不以调频或模拟器帧耗时替代电池测量。

真机对照应固定同一音源、曲目、亮度、音量及耳机/外放条件，分别运行30–60分钟，记录起止电量、供电状态及 `power:` 日志。插USB供电状态下的熄屏表现需要单独记录。
