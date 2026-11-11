# PSP 系统联网与插件兼容性核实

核对日期：2026-11-11。本文区分“系统联网界面”“WPA2兼容插件”“网络稳定性”，三者不是一件事。

## 本轮实际接入

- 联网页面按 △ 使用 `sceUtilityNetconfInitStart/GetStatus/Update/ShutdownStart`，控制器为 `src/wifi_dialog.c/h`。
- 非阻塞等待网络工作线程释放APCTL控制权，系统窗口结束到NONE前不接收应用按键。显示使用临时GU上下文，结束恢复原渲染后端。
- 结束后重新扫描系统已保存配置、清理旧DNS/空闲连接、重新验证HTTP可用性。系统窗口能展示什么扫描/新建项目由PSP固件决定，应用不会凭空添加SDK不存在的项目。
- 网络最终门禁位于`net.c`：极致省电拒绝新HTTP/DNS/连接/自动重连，并shutdown已登记socket，连接owner负责close，避免误关复用fd。
- 系统对话框拥有网络期间拒绝后台新连接，DNS迟到结果通过代次检查不进入新会话。

## 找到的WPA2插件

[Kethen/wpa2psp](https://github.com/Kethen/wpa2psp) 的README明确：代码来源于
[ARK-4的wpa2.c](https://github.com/PSP-Archive/ARK-4/blob/main/core/compat/psp/wpa2.c)，作者moment0253，许可遵循ARK-4（仓库元数据GPL-3.0）。

README明确该插件面向 **6.61 PRO**；ARK用户不需要另装，因为ARK已有对应功能。另有
[aethiopicuschan/wpa2psp4lme](https://github.com/aethiopicuschan/wpa2psp4lme) 为LME适配。

它补的是旧PSP网络栈的WPA2支持，不能据此宣称修复所有路由器掉线、DNS失败、HTTP超时或提高下载速率。
插件采用CFW/内核hook，不能作为普通用户态静态库塞入MELODY。不同固件不应混装或重复加载。
本轮没有对记忆棒seplugins配置做修改，也没有在用户固件未知时自动安装/加载内核PRX。

## 当前限制

- 已有系统对话框与应用网络取消/重连协调，不代表未知路由器与所有CFW实机均验证。
- PSP无线硬件的频段/协议/安全方式限制不会因应用UI变化消失。
- 若需要把WPA2补丁作为可选附件发布，先明确设备CFW（ARK/PRO/LME）、精确版本与插件许可，单独打包并做对应真机验收。
- 极致省电明确禁止联网，系统联网窗口及词库更新入口均不在该模式中开放。

## 上游证据

- [wpa2psp README](https://github.com/Kethen/wpa2psp/blob/main/README.md)
- [wpa2psp源码与GPL许可](https://github.com/Kethen/wpa2psp)
- [WPA2 PSP技术报道](https://wololo.net/2025/02/14/the-20-year-old-psp-can-now-connect-to-wpa2-wifi-networks/)
- [PSPSDK netconf接口](https://github.com/pspdev/pspsdk/blob/master/src/utility/psputility_netconf.h)

## 自动环境检测与6.60 PRO-C2

用户确认实机为6.60 PRO-C2无限。本轮新增`platform_plugins.c`只读自动检测：读取真实devkit版本、尝试枚举可见模块识别ARK/PRO/wpa2psp；内核模块不可见时明确CFW能力未确认，不把未见到模块当插件缺失或猜成PRO。
6.60环境不会自动加载上游仅标6.61 PRO的wpa2psp；已检测ARK或已加载WPA2也不重复安装。联网页SELECT重新检测并显示适配状态，日志记录完整结果。
目前没有经过6.60 PRO-C2真机验证的内置WPA2补丁，因此自动选择的结果可能是“无已验证匹配项”，这比错误加载6.61内核补丁更可靠。系统联网界面不依赖额外插件仍可使用。
