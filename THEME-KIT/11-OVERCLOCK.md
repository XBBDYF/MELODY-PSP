# PSP 外部超频接管

本功能默认关闭。它仅表示用户把时钟控制权交给外部 PSP Overclock Picker 插件；MELODY 不安装、加载或启动插件，不执行压力测试，不写 PLL，不承诺已实际达到某个频率。适用 PSP，未使用任何 PSVita 接口。

## 上游与许可

上游：https://github.com/mcidclan/psp-beyond-444mhz 。公开的 LICENSE.md 是 MIT，版权为 Copyright (c) 2026 m-c/d；本次仅根据接口行为独立编写桥接代码，没有捆绑上游实现或二进制。若后续复制插件源码，须一并保留完整许可。

已查阅的公开源码：

- https://github.com/mcidclan/psp-beyond-444mhz/blob/main/LICENSE.md
- https://github.com/mcidclan/psp-beyond-444mhz/blob/main/overclock-picker/plugin/README.md
- https://github.com/mcidclan/psp-beyond-444mhz/blob/main/overclock-picker/plugin/src/main.c
- https://github.com/mcidclan/psp-beyond-444mhz/blob/main/overclock-picker/plugin/src/overclock.h
- https://github.com/mcidclan/psp-beyond-444mhz/blob/main/overclock-picker/octable.h
- https://github.com/mcidclan/psp-beyond-444mhz/blob/main/overclock-picker/kernel/src/exports.exp

picker-plugin 是内核 PRX，依赖 CFW 的 libpspsystemctrl_kernel，钩住 display/exit。它没有提供应用可调用的设置频率或停用 API。独立 kcall.prx 导出的任意函数指针调用也不是超频控制接口；本桥接不调用它。

## 显式操作与支持范围

只有用户主动选择“外部超频接管（需插件）”并确认以下信息后才允许交接：

1. 设备为上游该插件支持的 1g、2g、早期 3g 或 5g。这里是硬件代数，不是 sceKernelGetModel() 的枚举值；调用方需正确转换。未知型号传 0，晚期 3000、Street 和未确认代数不接受。不能仅凭外壳“3000”认定为早期 3g。
2. 用户已经自行安装启用 opicker，并有来自同一设备、同一默认频率表的既有 opicker.id。应用不生成、猜测或修改此文件，不启动上游自动探测过程。桥接参数接受经调用方严格解析的 max_tested_index；该文件是上游最后保存索引，可能来自手动保存，不独立证明最大稳定性，是否来自同机及已有验证由用户确认。
3. 默认表为 PLL_DEFAULT_DEN=0x12，测试索引为 0..46。按照上游插件的 SAFETY_MARGIN=2，选择索引须低于已测最大索引至少两级。
4. 当前未启用 eco/极致等应用低频配置。调用方应通过 conflicting_power_profile 阻止冲突。

仅提供以下名义档位供用户对照插件手动选择，选择不会设置硬件：

| choice | 插件表索引 | 名义 MHz | 最低已测最大索引 |
|---|---:|---:|---:|
| 0 | 17 | 402.888 | 19 |
| 1 | 27 | 444.000 | 29 |
| 2 | 41 | 501.555 | 43 |

这些是默认表注释的名义值，并非独立测量、稳定性保证或精确的 400/500 MHz 请求。scePowerGetCpuClockFrequencyInt() 的返回值也不等于实际 PLL 频率或有效吞吐量。界面应明确使用“外部接管 / 期望名义档位 / 未验证实际频率”。

## 接入约定

桥接只有前台状态，无持久化；启动默认 OFF。enable(consent, choice) 成功只进入 EXTERNAL。EXTERNAL 与 RELEASE_PENDING 两状态的 owns_clock() 都返回真。此时 power 层应跳过全部 SDK 调频（包括初始化、tick、profile 切换、shutdown 恢复），并禁止软件 NULL 帧缓冲熄屏。上游 display hook 在快捷键激活绘制 OSD 时没有检查 NULL 帧缓冲，直接传 NULL 存在冲突。原生背光计时可以单独管理。

进入 eco/极致前若仍 owns_clock()，应拒绝切换并显示解除步骤；仅把软件布尔值关掉不足以交回硬件控制权。不要在启动时根据保存配置自动交接。

停用流程为 request_release() → RELEASE_PENDING → 用户实际取消外部插件超频 → acknowledge_release(1) → OFF。具体 deactivate_contract：用户在外部 opicker 中按住 L+R 并使用 VOL- 减到索引 0，或禁用插件后重启，再回应用确认。应用没有 API 停止插件，确认前保持外部控制权且不恢复 SDK 低频写入。确认是用户陈述，不代表应用验证了 PLL。上游 plugin module_stop 不等价于完整取消超频，也不应被当作释放接口。

休眠恢复调用 on_resume() 会把外部状态转为 RELEASE_PENDING，并清除显示名义档位的依据，但继续保留外部时钟控制权。由用户检查外部插件状态并按上述解除流程处理。普通退出也不应宣称已撤销外部插件；上游插件的 exit hook 负责其自身恢复，MELODY 不验证该结果。

## 验证边界

tests/test_overclock_bridge.py 使用主机 GCC 编译真实状态机，验证默认关闭、机型拒绝、显式确认、默认表索引边界、两级余量、配置冲突、释放待确认和恢复失效。它不执行 PSP 内核代码，不证明插件可在某台实机稳定超频。当前没有新增可编译 PRX，因为上游没有与本功能需求匹配的受限控制接口；直接捆绑完整插件会额外引入钩子和独立调频线程。
