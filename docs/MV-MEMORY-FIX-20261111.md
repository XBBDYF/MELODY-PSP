# 2026-11-11 MV 起播内存：预算核对与图片缓存回收

## 物理 PSP 原始证据

证据为 `.tmp/hardware-mv-memory-20261111.log:1618..1690`，属于本轮修订前构建。两次进入均显示：

```text
mv: gate #1 free=13344K max=13312K need=13664K prx=0 force=0
mv: blocked ... cause=total
mv mem: root-heap ... reserved=2048K
```

总空闲量少 320 KiB，最大连续块少 352 KiB；`cause=total` 只是代码先报告总量不足，不代表连续条件已满足。两次均未穿过 gate，因此 `#1` 是尚未启动首个 decoder 会话，不是已完成第一次播放。

音频 decoder 已退休，GE 已释放 768 KiB，皮肤 Lua heap 38520 字节已挂起，图片缓存日志为 0。API worker 已停止且走过 `api_free_buffers()`。32 MiB 是用户给定机器可用 RAM 档位，不应把它当作 gate 时仍空闲的内存。

## gate 计数边界

`src/mv.c` 的 gate 在音频、API、GE、skin 释放之后、`g_host->mv_load()` 之前采样。`prx=0` 时将 decoder 模块镜像与其稍后懒申请的 heap 一起预留是合理的，未看到将已经占用的主程序 root heap 再次加到 `need` 的计算；主程序 2 MiB heap 已体现在 `free/max` 中。这里的 `need` 是固定配置常量，并无逐帧累计分配的算法。

正式软件多格式构建 `tools/prxloader/build_software_players.sh:22` 指定 `MV_HEAP_KB=7168`；`tools/prxloader/mvprx.c` 默认 8 MiB 用于未覆盖宏的其他构建。`src/mv.c` 的旧长注释仍用 4.7 MiB 镜像、8 MiB heap 描述历史版本，不能将它当成本轮精确预算。

当前 gate 注释采用 6212 KiB 镜像、7168 KiB heap、256 KiB `mv_play` 栈，三者相加 13636 KiB，13664 KiB 常量还剩 28 KiB 余量。6212 KiB 属于已有源码注释证据，最终镜像实际值须由本轮 ELF/装载实测确认。若 decoder 有约 4 MiB 静态 global/BSS，它属于模块装载内存镜像，只有确认不包含在镜像尺寸内时才另计；不能在镜像预算上重复加它。decoder heap 内部的 malloc/av_malloc 同样消耗这份 7168 KiB reservation，不应再把它们全部加到 gate。

保留 `MV_MIN_FREE_FIRST_KB=13664` 与连续块检查，不修改 `mv.c/h`，不添加强制放行或降门槛。

## 已修改的真实常驻成本

`src/api.c` 原有两个静态像素缓存：

| 数组 | 每项字节 | 项数 | 合计 |
| --- | ---: | ---: | ---: |
| `g_thumbs` | 25664 | 24 | 615936 B = 601.5 KiB |
| `g_avs` | 5184 | 24 | 124416 B = 121.5 KiB |
| 合计 | | | 740352 B = 723 KiB |

旧 `api_free_buffers()` 释放 JSON/JPEG/RGBA/art/backdrop 等，但这两份 BSS 随主程序模块永久常驻，即便当前缓存内容为零也占用内存。skin image cache 的零值不涵盖 API 的这两份数组。

现将它们移至单独 `PSP_SMEM_High` 分区 `melody_images`，不挤占固定 libc heap。`api_init()` 在启动 worker 及本地封面恢复前申请、清零；申请失败只使缩略图/头像接口返回缺图，不解引用空指针。`api_free_buffers()` 在既有 MV worker 退休流程之后释放该分区，并清队列、递增图片代次、清所有指针。存活 worker 时额外保留此分区，避免释放批次仍可能写入的像素。UI 返回的像素指针与现有 art 一样只在 API 缓冲释放前有效；MV 期间不保留这些指针用于重绘。

`api_shutdown()` 单独调用仍保留缓存，保持普通 worker 停启时的就绪封面复用。实际 MV 缓冲释放后，下一次 `api_init()` 重建冷缓存，并恢复磁盘封面。

从静态主模块移走的像素空间为 723 KiB；高分区运行态占用在 gate 前释放。该数值超过旧日志两种缺口，但不把它等同于已测量的连续空闲块增量；最终地址布局、同时新增常驻数据及新 IME snapshot 释放均需重链接后实测。

## 验证与交接

已在主机运行：

- `python tools/tests/test_api_image_cache_memory.py`：抽取实际生产定义、生命周期 helper、slot/pixel accessor 与请求代次检查，验证实际 `sizeof(ImageCacheStorage)==740352`、分区申请失败、head 查询失败、重复申请、worker 存活禁止释放、释放幂等、队列/指针清空及两次重建。
- `python tools/test_home_thumb_cache.py`：保持原有 12 个真实封面 URL/hash 的合成 RGB565 磁盘恢复、ready 复用、缺文件/碰撞/截断断言；仅把 fixture 改为为生产指针提供 backing storage，清空尺寸继续覆盖整个数组。

本子任务不改 main、ime、构建脚本，不执行 Git、网络或装卡；PSP 编译、重链接、全量回归与物理 PSP 两次 MV 由主代理统一进行。在收到新实机 gate 数据之前，不宣称内存问题实机解决。
