# 关键逻辑说明

本文按「用户做了什么 → 程序内部发生了什么」的方式描述主要链路。
涉及的文件都在 `src/` 下，函数名可直接搜索。

---

## 1. 启动与联网

```
prx_entry.c: module_start
  └─ 创建 melody_main 线程 → main()
       ├─ log_open()              滚动日志写到 ./log.txt（限制大小）
       ├─ store_init()            读设置/最近/收藏
       ├─ cache_init()            读 cache/index
       ├─ gfx_init()              显存初始化，选 GE/GU
       ├─ audio_init()            起输出/解码线程（此时空转）
       ├─ net_init()              WLAN 驱动 + APCTL 注册
       ├─ api_init()              起 melody_api 线程
       ├─ dl_init()               起 melody_dl 线程
       └─ 进入主循环，SC_BOOT → 自动连接
```

自动连接（`job_probe` 链路）：

1. `net_last_profile()` 读上次成功的配置号，先试它；
2. 失败则遍历 `net_list_profiles()` 的**全部**已配置项（不写死 1..3）；
3. `net_apctl_connect()` 等 `GOT_IP`，再等 DNS 服务器下发（最多 6 秒）；
4. 用一次真实 HTTP 请求确认「能拿到 JSON」，而不是只看 TCP 三次握手成功；
5. 成功后写入 `profile号`，下次开机优先。

> 实测教训：只看 `GOT_IP` 会在 DHCP 还没下发 DNS 时开始解析，第一首歌因此白等 11～46 秒。

---

## 2. 搜索

```
用户输入 → api_request_search(q)
   ├─ job_search：GET /api/cloudsearch/pc?s=<q>&type=1&limit=<N>&offset=<off>
   │    ├─ list_cb 把每首歌写入 g_pending（Song）
   │    └─ 第一页额外调用 alt_kuwo_search_songs()      ← 第三方补充
   │         · 酷我搜索是纯 HTTP，返回原唱条目
   │         · 解析时丢弃「片段/铃声/伴奏/纯音乐/KTV/试听」和超长合辑
   │         · 结果带 playable=2、fee=-1，界面标「酷我」
   ├─ 去重（同名同歌手）后合并，投 EV_LIST_OK → 主线程
   └─ 主线程 copy_list_from_api() 复制到 g_list，再 sort_search_list()
```

排序（`main.c:song_score`）：

| 条件 | 分数变化 |
|---|---|
| 名称含翻唱/改版/伴奏/Live/演唱会/片段… | −150 |
| 歌手包含查询词（搜「周杰伦」时歌手是周杰伦） | +90 |
| 名称包含查询词 | +40 |
| 第三方音源行（确定能播） | +30 |
| 网易标记可播 | +10 |
| 底分 | 热度 `pop` |

稳定插入排序，分数先算一次存表，避免 O(n²) 里重复做字符串匹配。

---

## 3. 播放

`main.c` 里只有两条入口：`play_index()`（当前列表）与 `play_song_obj()`（任意一首）。
真正的取址在 API 线程：

```
job_play(index, song)
  ├─ 1) 缓存命中（cache_complete）→ 直接 audio_play_cached()，不发网络请求
  ├─ 2) playable==2（网易无地址）→ alt_resolve() 先试，失败再走官方接口
  ├─ 3) resolve_url(id)：/api/song/enhance/player/url
  │       ├─ 品质：默认 128k，历史失败会降级（api_note_play_fail）
  │       └─ 45x 错误 → alt_resolve()
  ├─ 4) alt_resolve()：byfuns → 酷我 → 本地网关 → GDStudio（用户可在设置里开关）
  └─ 5) 拿到 url 后 audio_play_stream(song, url, total)
```

`audio_play_stream()` 之后：

```
dl_stream_start(song, url, total, gen)   ← 把「取流」交给 dl 线程
decode 线程：
  wait_data(gen, 0, 1)       等 dl 线程先建文件、写出第一个字节
  detect_id3 / 找第一帧
  预缓冲约 1.5 秒 → 开始播放
  播放中：ring buffer 不足 → BUFFER；解码失败 → 重同步；连续失败 → AUDIO_ERROR
```

事件：`EV_PLAY_OK`（起播成功）、`EV_PLAY_FAIL`（取址失败）。
主线程的重试策略：**只有真正播放满 2 秒才算起播成功**，否则最多重试 2 次
（每次间隔 1.5 秒），仍失败则暂停并提示「按○重试」，不会无限循环重试。

自动下一首：`AUDIO_END` 且播放位置接近时长尾部 → `play_next(1)`；
播放模式（单曲/列表/随机）在 `next_plan.h` 里计算，预取队列按它生成。

---

## 4. 下载

```
用户选中若干首 → dl_enqueue(id, song)
dl 线程：
  job_begin：建 cache/<id>.mp3，写 .part 标记（失败重试 4 次）
  分片：默认 4 条连接（设置 2..6），Range 请求，每片写自己的偏移
  每 64KB 批量 flush，`.part` 里保存「已校验前缀 + 各片进度」
  完成：校验总长度 → 改名发布 → cache_add(id) → EV_CACHE_DIRTY
```

三条重要规则：

1. **可读前缀**：`job_prefix()` 计算「从头开始连续写完」的字节数，
   边下边播只允许读这段，避免读到空洞。
2. **断点续传**：`.part` 记录每片偏移与前缀哈希；重连后从原偏移继续，
   不重下已校验部分。
3. **优先级**：MV 播放时 `dl_set_video_priority(1)` 让下载队列让路；
   更新安装期间只提高更新任务优先级，结束后恢复。

进度显示：百分比一律由**字节数**换算（`dl_progress_contract.h`），
不再用「已写块数 / 总块数」估算，避免出现 100% 却未完成。

---

## 5. MV

```
mv_open(song) → mv_resolve() 取 MV 地址（网易 /api/song/enhance/mv/url）
  ├─ 在线流：mv_stream_enabled() ? mvsrc 快速取流 : 下载完再播
  ├─ 起播门槛：实测码率必须 ≥ 1.25 × 视频码率，否则先缓冲
  │    （连接页显示：速率、所需带宽、已缓冲、剩余时间）
  └─ 解码：
       ├─ AVC 原生模块（melody_mv_avc.prx）—— 硬解，兼容性有限
       └─ 软件模块（melody_mv565[_fast|_balanced|_sharp].prx）—— 四档画质
```

本地 MV：设置项「本地 MV 搜索」控制是否扫描 `ms0:/VIDEO`，默认关闭
（扫描大目录会明显拖慢启动）。

---

## 6. 皮肤

```
启动：skin_runtime_init() 读 store 里的主题 id → skin_runtime_apply(./skins/<id>)
  ├─ validate：目录结构、skin.json、Lua 语法
  ├─ 建 VM：栈/堆上限固定，注册 ui.* / text.* / input.* 等宿主函数
  └─ 每帧：skin_runtime_draw(page, t, dt) → skin.lua 的 draw(page, m)
输入：skin_runtime_input(page, pressed, held, dt) → 返回动作字符串
动作：main.c 执行（如 "play"、"next"、"open_settings"）
```

皮肤安装（设置 → 主题 → 在线主题）：

```
skin_catalog_refresh()  GET melody/skins/index.json（COS）
skin_catalog_install(id)：
  下载 zip → 解到 skins/<id>.tmp → 校验 → 改名发布（rename 是原子的）
  mkdir/write 失败会重试 3 次（每次 25/50ms），失败原因写进 skin_catalog_install_error()
```

主题页会在列表项下方直接显示失败原因，而不是只弹一个「安装失败」。

---

## 7. 在线更新

```
启动静默检查：GET melody/version.txt
  ├─ 第一行版本号 > APP_VER
  ├─ 第二行 EBOOT.PBP 地址，第三行字节数，第四行 crc32
  └─ 说明行（第 5 行起）= 更新日志，弹窗里滚动显示
用户确认 → 下载 EBOOT.PBP 到 ./MELODY.PBP → 校验 crc32 → 重启进入安装
安装（启动器）：校验 0-20% → 逐模块解压 20-90% → 替换提交 90-100%
```

更新图库（更新后首次启动的展示页）：

```
update-preview-pending.txt（版本 + revision）→ 请求
  melody/update-preview/<version>/index.json
  校验 schema=1、版本、revision（= 安装包的 bundle crc32）
  下载 01.png/02.png 到 ./update-preview/ 并显示
```

---

## 8. 设置与状态存储

- `store.c`：`settings.txt`（25 个开关，顺序由 `settings_order.h` 统一）、
  `recent.txt`、`fav.txt`、`cache/index`、`alt.txt`（备用音源）、`dns.txt`（DNS 缓存）。
- 写入策略：先写 `.tmp` 再 `rename` 发布，掉电不会留下半截文件。
- 设置项顺序是「单一来源」：`settings_order.h` 的数组同时驱动界面顺序、
  上下键移动与 `MELODY_SETTINGS_COUNT`，测试会断言三者一致。

---

## 9. 诊断

- `log.txt`：滚动日志（超过上限从头覆盖），关键节点都带毫秒时间戳；
  网络、DNS、下载、解码、皮肤、更新各有前缀（`net dns`、`dl queue`、`audio`、`skin io`、`update`）。
- 设置 → 关于 → 日志：可直接查看/导出，方便群里反馈。
- 日志里常见可读信号：
  - `dns http ok <host>`：系统 DNS 失败，走了 HTTP 备用解析；
  - `audio error, retry N: <原因>`：解码/取流失败；
  - `dl stream: begin failed … err=…`：下载流没起来，err 是具体原因。
