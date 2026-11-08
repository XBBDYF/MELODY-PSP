# 关键逻辑说明

本文按「用户做了什么 → 程序内部发生了什么」的方式描述主要链路。
涉及的文件都在 `src/` 下，函数名可直接搜索。

---

## 1. 启动与联网

```mermaid
flowchart TD
    A[module_start] --> B[创建 melody_main 线程]
    B --> C[log_open 滚动日志]
    C --> D[store_init 设置/最近/收藏]
    D --> E[cache_init 读 cache/index]
    E --> F[gfx_init 显存初始化 GE/GU]
    F --> G[audio_init 起输出/解码线程]
    G --> H[net_init WLAN + APCTL]
    H --> I[api_init 起 melody_api 线程]
    I --> J[dl_init 起 melody_dl 线程]
    J --> K[主循环 SC_BOOT]
    K --> L[自动连接 job_probe]
```

启动顺序如上：**先本地状态，再显存，最后网络**。这样即使没插网卡/没配 WiFi，
程序也能进界面（设置、缓存、本地音乐都可用），不会卡在启动阶段。

自动连接（`job_probe` 链路）：

1. `net_last_profile()` 读上次成功的配置号，先试它；
2. 失败则遍历 `net_list_profiles()` 的**全部**已配置项（不写死 1..3）；
3. `net_apctl_connect()` 等 `GOT_IP`，再等 DNS 服务器下发（最多 6 秒）；
4. 用一次真实 HTTP 请求确认「能拿到 JSON」，而不是只看 TCP 三次握手成功；
5. 成功后写入 `profile号`，下次开机优先。

> 实测教训：只看 `GOT_IP` 会在 DHCP 还没下发 DNS 时开始解析，第一首歌因此白等 11～46 秒。

---

## 2. 搜索

```mermaid
flowchart TD
    Q[用户输入] --> R[api_request_search q]
    R --> NE[网易 cloudsearch<br/>第一页 limit/offset]
    NE --> P1[list_cb 写入 g_pending]
    P1 --> K{第一页?}
    K -- 是 --> KW[alt_kuwo_search_songs<br/>酷我纯 HTTP 搜索]
    KW --> F[丢弃片段/铃声/伴奏/纯音乐/KTV/超长合辑]
    F --> DED[按 同名+同歌手 去重后追加<br/>playable=2, fee=-1]
    K -- 否 --> DED2[直接进入合并]
    DED --> CP[EV_LIST_OK → 主线程]
    DED2 --> CP
    CP --> COPY[copy_list_from_api 复制到 g_list]
    COPY --> SORT[sort_search_list<br/>原唱优先 + 稳定插入排序]
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

```mermaid
flowchart TD
    A[play_index / play_song_obj] --> B[api_request_play]
    B --> C{缓存完整?}
    C -- 是 --> C1[audio_play_cached<br/>不发网络请求]
    C -- 否 --> D{playable == 2<br/>网易无地址?}
    D -- 是 --> E[alt_resolve 先试]
    D -- 否 --> F[resolve_url<br/>enhance/player/url]
    F -- 45x / 无地址 --> E
    F -- 成功 --> G[拿到直链]
    E --> G
    C1 --> H[audio_play_stream]
    G --> H
    H --> I[dl_stream_start<br/>交给 dl 线程取流]
    I --> J[解码线程 wait_data → 找首帧]
    J --> K[预缓冲约 1.5s → 播放]
```

取址细节（按优先级）：

1. 缓存命中（`cache_complete`）→ 直接 `audio_play_cached()`，不发网络请求；
2. `playable==2`（网易无地址）→ `alt_resolve()` 先试，失败再走官方接口；
3. `resolve_url(id)`：`/api/song/enhance/player/url`，默认 128k，历史失败会降级；
4. `alt_resolve()`：byfuns → 酷我 → 本地网关 → GDStudio（设置里可开关）；
5. 拿到 url 后 `audio_play_stream(song, url, total)`。

`audio_play_stream()` 之后：

```mermaid
flowchart LR
    A[dl_stream_start<br/>建文件 / 写首字节] --> B[解码线程 wait_data]
    B --> C[detect_id3 / 找首帧]
    C --> D[预缓冲 1.5s]
    D --> E[播放]
    E --> F{环形缓冲不足?}
    F -- 是 --> G[BUFFER 等待]
    G --> E
    E --> H{解码失败?}
    H -- 单帧 --> I[重同步]
    I --> E
    H -- 连续失败 --> J[AUDIO_ERROR → 主线程重试]
```

事件：`EV_PLAY_OK`（起播成功）、`EV_PLAY_FAIL`（取址失败）。
主线程的重试策略：**只有真正播放满 2 秒才算起播成功**，否则最多重试 2 次
（每次间隔 1.5 秒），仍失败则暂停并提示「按○重试」，不会无限循环重试。

自动下一首：`AUDIO_END` 且播放位置接近时长尾部 → `play_next(1)`；
播放模式（单曲/列表/随机）在 `next_plan.h` 里计算，预取队列按它生成。

---

## 4. 下载

```mermaid
flowchart TD
    A[选中若干首 → dl_enqueue] --> B[job_begin<br/>建 cache/id.mp3]
    B --> C[写 .part 标记<br/>失败重试 4 次]
    C --> D[分片：默认 4 连接<br/>设置 2..6，Range 请求]
    D --> E[每 64KB 批量 flush]
    E --> F[.part 保存<br/>已校验前缀 + 各片进度]
    F --> G{整首完成?}
    G -- 否 --> D
    G -- 是 --> H[校验总长度]
    H --> I[改名发布 + cache_add]
    I --> J[EV_CACHE_DIRTY → 刷新界面]
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

```mermaid
flowchart TD
    A[mv_open song] --> B[mv_resolve<br/>enhance/mv/url]
    B --> C{在线流开启?}
    C -- 是 --> D[mvsrc 快速取流]
    C -- 否 --> E[下载完再播]
    D --> F{实测速率 ≥ 1.25 × 视频码率?}
    E --> F
    F -- 否 --> G[继续缓冲<br/>连接页显示速率/所需带宽/剩余时间]
    G --> F
    F -- 是 --> H{解码路径}
    H -- AVC 原生 --> I[melody_mv_avc.prx<br/>硬解，兼容性有限]
    H -- 软件模块 --> J[melody_mv565*.prx<br/>四档画质]
    I --> K[显示]
    J --> K
```

本地 MV：设置项「本地 MV 搜索」控制是否扫描 `ms0:/VIDEO`，默认关闭
（扫描大目录会明显拖慢启动）。

---

## 6. 皮肤

```mermaid
flowchart TD
    A[启动] --> B[skin_runtime_init 读主题 id]
    B --> C[skin_runtime_apply ./skin_release/melody/skins/id]
    C --> D{validate: 目录/skin.json/Lua 语法}
    D -- 失败 --> E[退回原生界面 + skin_runtime_error]
    D -- 通过 --> F[建 VM，注册 ui.* / app.*]
    F --> G[每帧: skin_runtime_draw page,t,dt]
    G --> H[skin.lua draw page, model]
    H --> I[skin_runtime_input → 动作字符串]
    I --> J[main.c 执行: play/next/open_settings]
```

皮肤执行约定：

- `draw(page, m)` 每帧调用，`m` 是宿主打包的数据表（页面、播放状态、列表窗口等）；
- `input(page, pressed, held, dt)` 返回动作字符串，主程序统一执行，皮肤不直接改状态；
- VM 有固定内存与「字符串工作量」预算，超限回退原生界面而不是崩溃；
- 危险全局函数（`dofile`/`load`/`pcall`/`setmetatable`）在初始化时被移除。

皮肤安装（设置 → 主题 → 在线主题）：

```mermaid
flowchart TD
    A[skin_catalog_refresh] --> B[GET melody/skins/index.json]
    B --> C[用户选择安装]
    C --> D[下载 zip 到 skins/id.tmp]
    D --> E{解压与校验}
    E -- 失败 --> F[重试 3 次 / 25 与 50ms]
    F -- 仍失败 --> G[skin_catalog_install_error 显示原因]
    E -- 成功 --> H[rename 发布 skins/id]
    H --> I[皮肤列表刷新并可选]
```

主题页会在列表项下方直接显示失败原因，而不是只弹一个「安装失败」。

---

## 7. 在线更新

```mermaid
flowchart TD
    A[启动静默检查] --> B[GET melody/version.txt]
    B --> C{第一行版本 > APP_VER?}
    C -- 否 --> Z[结束]
    C -- 是 --> D[弹窗显示更新日志<br/>第 5 行起逐条滚动]
    D --> E{用户确认?}
    E -- 稍后 --> Z
    E -- 更新 --> F[下载 EBOOT.PBP 到 ./MELODY.PBP]
    F --> G{crc32 校验}
    G -- 失败 --> H[提示校验失败，不安装]
    G -- 通过 --> I[重启进入安装器]
    I --> J[校验 0-20%]
    J --> K[逐模块解压 20-90%]
    K --> L[替换与提交 90-100%]
    L --> M[保留设置/缓存/队列，写入新版本]
```

更新图库（更新后首次启动的展示页）：

```mermaid
flowchart LR
    A[update-preview-pending.txt<br/>版本 + revision] --> B[GET update-preview/version/index.json]
    B --> C{schema=1 且 版本/revision 匹配?}
    C -- 否 --> D[放弃显示，保留原生界面]
    C -- 是 --> E[下载 01.png / 02.png 到 ./update-preview/]
    E --> F[首屏展示本次更新内容]
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
