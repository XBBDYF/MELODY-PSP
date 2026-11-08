# 测试说明

本目录的测试全部在 **PC 上**运行，不需要 PSP。做法是把 `src/` 里的
纯逻辑（多数通过 `*_contract.h` 暴露）切出来，配一层平台桩，用宿主机 gcc
编译成小程序后断言行为。

```bash
python -B tests/test_dl_progress.py          # 跑一个
python -B tests/run_all.py                   # 跑全部（需 Python 3.9+）
```

## 两类测试

| 目录 | 含义 | 默认是否运行 |
|---|---|---|
| `tests/test_*.py` | 自包含单元/契约测试：只需 `src/` 与 gcc | 是 |
| `tests/integration/test_*.py` | 需要额外产物才能跑（WiliMini 解码树、已构建的发布包、本机浏览器、Python wheel 等） | 否 |

`tests/integration/` 里的用例说明了具体缺什么：

- `test_mv_receive_scheduling.py`：需要 WiliMini 的 `tools/mvd/patched/decoder/` 头文件；
- `test_mv_tail_recovery.py`：同上，需要解码器源码树；
- `test_pc_login_release.py`：需要本机 Chrome/Edge 与 `deps/*.whl`（走 CDP 导出真实 Cookie）；
- `test_update34.py`、`test_update_bulk_write.py`：需要 `releases/` 下已构建的安装包。

## 写新测试

1. 能写成纯函数就写成纯函数，放进 `src/*_contract.h`；
2. 切片时用**完整函数签名**做锚点（签名改了测试要一起改，这是刻意的）；
3. 断言要能描述「用户能感知的行为」，而不是内部实现细节；
4. 涉及时序（重试、DNS、缓冲）的用例要用**虚拟时间**，不要 `sleep`。

参考：

- `test_dl_progress.py` —— 纯契约（百分比/字节换算）
- `test_dns_http_fallback.py` —— 带桩的模块切片
- `test_music_failure.py` —— 把主循环的重试代码块切出来跑
- `test_buffered_exit_replay.py` —— 在生产函数上跑真实文件 I/O（最复杂的一类）
