# 参与开发

## 先读这个

- [工作流程](docs/WORKFLOW.md)：开发 → 测试 → 打包 → 发群 → 同步开源
- [GitHub 记录](docs/GITHUB.md)：仓库信息、推送方式、Issue 约定

## 先跑起来

1. 读 [docs/BUILD.md](docs/BUILD.md) 配好 pspdev 与外部依赖。
2. `make` 能出 `melody.prx`，`python -B tests/test_net_boot_connect.py` 能跑通。
3. 有 PSP 最好；没有的话 PPSSPP 可以验证大部分界面逻辑（`ppsspp-smoke` 里放一个带 `portable.txt` 的目录即可）。

## 改动约定

- **C 代码**：C99，2 空格缩进，不用 C++ 特性；与 PSP SDK 直接交互的代码集中放在模块文件里，
  纯逻辑尽量放进 `*_contract.h` 之类的头文件，方便宿主机测试。
- **内存**：PSP 只有 32MB，新增静态数组前先算账；大对象必须能释放回堆。
- **线程**：不要让主线程阻塞在网络/文件上；跨线程共享数据用代次号或锁。
- **日志**：关键分支要有 `mlog()`，带上可判定的量（毫秒、字节数、错误码），
  群里反馈问题时第一句话通常是「日志发一下」。
- **主题**：皮肤改动请同时跑 `python -B tools/validate_skin_runtime.py skin_release/melody/skins/<id>`；
  `metro` 与 `metro_day` 共用脚本，改完两边都要同步。

## 测试

新增功能请补 `tests/test_*.py`：

```bash
python -B tests/test_your_feature.py
```

测试的写法是「把 `src/` 里的函数片段切出来 + 提供桩 + gcc 编译 + 断言」，
参考 `tests/test_dl_progress.py`（纯契约）与 `tests/test_search_original_rank.py`（切片）两种风格。

时序类改动（播放重试、DNS、下载进度）**必须**有宿主机用例，因为这些 bug 在模拟器上不一定复现，
在真机上又复现代价太高。

## 提交

- 一个提交做一件事；提交信息写「现象 + 根因 + 改动」，例如：

  ```
  修复：重启后第一首歌白等十几秒
  根因：GOT_IP 早于 DHCP 下发 DNS，首次解析同步阻塞
  改动：net_apctl_connect 增加 DNS 就绪等待；解析移入有界 worker
  ```

- 涉及真机行为的改动，请说明验证环境（PPSSPP / 物理 PSP 型号 + 固件版本），
  不要把模拟器结论写成真机结论。

## 不要提交

- 编译产物（`*.o` `*.prx` `*.PBP` `*.elf`）
- `releases/` `dist_*/` `backups/` `.tmp/` 等构建与备份目录
- 个人日志、记忆棒备份、缓存的音乐文件
- 任何音乐/视频内容（版权问题）

