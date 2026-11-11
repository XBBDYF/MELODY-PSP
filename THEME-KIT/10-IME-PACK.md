# IME 纯数据词库包

本模块是二进制数据包，不是 PRX，也不执行服务器提供的代码。内置词库始终可用；可选词库路径是 `APP_DIR/ime/current.bin`。网络下载与更新设置由调用层负责。

## 主线程与 worker 接口

`src/ime_pack.h` 提供不共享全局状态的数据包操作：

- `ime_pack_validate(path)`：流式检查整个文件，成功为 0，失败为负数。
- `ime_pack_prepare(path, &prepared)`：完整校验并打开独立只读句柄；失败时输出 NULL。成功后调用方拥有句柄。
- `ime_pack_install(download_path, current_path)`：worker 串行调用，复制到同目录 `current_path.install`，校验复制完成的文件，再以一次替换发布。失败不删除 current。目录须已存在，源/目标应是应用内部受控路径。
- `ime_pack_close(prepared)`：关闭尚未移交主线程的句柄。

`src/ime.h` 提供主线程接口：

- `ime_load_dictionary(APP_DIR)`：启动先读 `ime/active.txt` 单行 basename 并加载指向的包，失败再尝试 `ime/current.bin`，最后激活内置库并返回原因。basename 最多 80 字节，仅 ASCII 字母、数字、点、下划线、连字符；拒绝 `.`、`..`、嵌入 NUL、目录分隔符和额外行，允许单个末尾 LF/CRLF。PSP 指针读取同样使用 sceIo，不访问 newlib FILE。
- `ime_activate_pack(prepared)`：接管 prepared 所有权，重置输入、候选及旧字库资源。传 NULL 选择内置库。必须仅在主线程输入/绘制之间调用。
- `ime_dictionary_revision()`：返回数据版本，内置库为 0。

推荐 PSP 热更新使用不可变唯一文件名（如 pack-<revision>-<timestamp>.bin）：worker 下载至自己的临时文件 → install 到新文件名 → prepare 新文件 → 更新调用层管理的小 active.txt 指针 → 经有同步保证的事件/队列移交 prepared → 主线程空闲处 activate。worker 不访问已激活句柄，也不关闭已移交的指针。安装与 prepare 使用自己的句柄，候选查询仅由主线程使用 active 句柄。PSP 分支全程使用 sceIoOpen/Read/Write/Lseek32/Close/Rename/Remove，避开项目非线程安全的 newlib FILE 链表，不持有全局 stdio 锁。worker 使用 APP_DIR 相对路径前仍须按应用约定绑定工作目录。install 从不修改 active 指针，避免并发 UAF。事件未被消费、取消或程序退出时，调用层负责 close 尚未激活的 prepared。

不要在 install 前关闭当前活动库。即使新包安装或 prepare 失败，当前主线程仍可使用旧库。文件已被打开后必须视为不可变：只能通过 install 替换目录项，不能原地覆盖 current 文件内容。一个 installer 同时运行；不要多个 worker 写同一个 `.install`。

Windows 使用共享删除的读句柄及 `ReplaceFileA` 发布，首次创建回退到 `MoveFileExA`。PSP 使用 `sceIoRename(temp,current)`，其他平台使用标准 C `rename(temp,current)` 的单步替换；若平台驱动拒绝覆盖已存在或已打开文件，返回 IO 错误并保留旧包，不采用“先删旧文件”流程。PSP 更新层使用新唯一文件名和 active.txt 指针可避免替换已打开旧包的驱动限制；包模块启动读取支持该指针。Windows NTFS 已实测打开旧句柄后替换仍能读取旧数据；PSP FAT 驱动及断电持久化语义尚未实机验证。CRC 用于检测损坏，不承担来源认证；下载来源可信性由更新层负责。

## 候选与窗口

`ime_candidate_count()` 保留签名，但返回全部候选数；`ime_candidate_total()` 与它一致。512 仅保留为旧缓冲常量，已经不是总数限制。整词也不再被 collect 的前 32 项截断。

- `ime_candidate_window(first, out[][IME_WORD_MAX], max)` 将任意索引窗口复制给 UI。建议每帧最多 4 项，窗口起点可使用当前选中位置。
- `ime_candidate(i)` 与 `ime_select(i,...)` 支持完整全局索引，包括 512 之后的位置。
- `ime_candidate()` 返回 8 槽暂存文本，后续查询可能覆盖，不能跨 reset/activate 持有。对主题及跨层数据应使用 window 复制，不向 Lua 每帧展开全部候选。
- 排序为精确整词、对应音节单字、前缀词；每组保留全部匹配记录，按频率排序。同一前缀下的多音单字做 Unicode 去重，所有已收录读音均可访问。

候选使用 8 字节的记录引用/排序分数，不把所有词复制到 RAM。引用数组按实际匹配数增长并复用，直到切换词库才释放。当前 `s` 有 4,610 项，占 36,880 字节；包格式理论上限 65,535 项，引用数组不超过 524,280 字节。realloc 的短暂双份内存仍须纳入应用整体 2 MB 堆预算；分配失败返回空候选，保留 preedit 原文可提交。静态单字去重位图为 8 KB，每个包句柄另有 4,080 字节（51 条记录）缓存页，校验及顺序候选查询按页读取，减少逐条 seek/read；PSP SDK 每次读取最多一页，host 继续使用 C stdio。完整数据包不会进入堆。

查询先对按 key 排序的区间二分，再顺序收集匹配记录和排序。当前实现用一次计数及一次填充，不扫描无关 key 区间；可见窗口仅读取显示条目。字符前缀仅在不是完整音节时扩大到相同前缀的音节。联想 `ime_assoc` 的非拉丁文字前缀仍顺序扫描中文词区；调用层应在提交后调用一次并缓存结果，避免每帧执行。宿主机 `s` 查询加窗口/选择 CPU 为约 0.008 秒；PSP 存储延迟尚未测量。

## 数据布局 v1

所有整数均为无符号 32 位小端。64 字节头后是固定 80 字节记录，没有指针和可执行段。

| 偏移 | 内容 |
|---:|---|
| 0 | 8 字节 magic：`MIMEPK1\0` |
| 8 | 格式版本，必须为 1 |
| 12 | 头大小，必须为 64 |
| 16 | 文件总字节数，必须与实际完全一致 |
| 20 | 后续全部记录的 IEEE CRC32 |
| 24 | 总记录数，1..65535 |
| 28 / 32 / 36 | 拼音词 / 日语词 / 单字字音记录数，合计必须等于总数 |
| 40 | 记录大小，必须为 80 |
| 44 | 数据 revision，由服务端版本管理；不是格式版本 |
| 48..63 | 保留，必须为 0 |

记录为 `key[48] + word[24] + freq:u32 + kind:u32`。key 和 word 都必须非空、NUL 结尾、剩余填零，UTF-8 必须有效；拒绝过长、代理区、非最短编码、控制字符、缺少字形的文本。freq 必须大于 0。kind 为 0（拼音词）、1（日语词）、2（单字字音）；拼音和单字 key 仅 `a..z`，单字必须恰好一个 Unicode 字符。记录按 `(kind, key, -freq, word)` 严格排序，重复或乱序会被拒绝。计数、尺寸和读取索引均校验，最大文件为 5,242,864 字节。

字形白名单由实际内置 `src/font16.bin` 生成至 `src/ime_font_data.h`，运行时校验每个候选字形；因此热更新包不能悄悄加入在内置字体中显示成方框的字符。ASCII 可打印字符始终允许。主题额外字体不能扩大这一通用保证，新增字形须同步更新内置字体与生成的白名单。

## 生成与词语覆盖

运行：

```text
python tools/build_ime_dict.py
python tools/build_ime_dict.py --pack path/to/current.bin --revision 20261112
```

生成默认 `tools/ime/current.bin`、内置 `src/ime_dict_data.h` 及字体覆盖表。输出先写 `.building` 再替换，方便服务器发布前审阅。无需编译 PRX 即可发布后续数据包；字体白名单或格式改变才需应用升级。

扩展包读取现有 MIT jieba 词频原数据，保留最高频的 32,000 个可显示中文词，并加入人工词表。内置 fallback 维持约 8 千条词的规模以控制程序映像，单字字音完整保留。当前包为 59,486 条/4,758,944 字节：32,156 中文词、45 个满足 23 字节上限的日语词、27,285 条字音。旧日语表中超过 word[24] 的条目此前运行时也会被丢弃，现在在生成阶段过滤。

人工读音 `tools/ime/pinyin.txt` 采用 `词语|无调全拼连写|1..9`，覆盖自动逐字首读音。已明确加入 `视奸/时间/事件/实践 → shijian`、`初音 → chuyin`、`初音未来 → chuyinweilai` 以及 V 家专名、银行/行业/行为、重庆/重新/重量、长大/长江、快乐等多音词。可新增可选 `tools/ime/user.txt`，格式相同，按词覆盖自动词条，适合服务器追加用户专名。用户词必须满足字体与字节预算，重新生成包即可；当总记录数超过 65535，生成器明确报错。

jieba 自动扩词仍采用逐字首读音，未经人工覆盖的多音词并不保证正确；它不声称提供语言级自动多音消歧。单字则保留 pinyin-data 中每一个已收录读音。两份上游 MIT 来源及其许可继续保留在 tools/ime 原目录。

## 验证证据

先写 `tests/test_ime_pack.py`，旧代码实测在 `has("视奸")` 断言失败；修复后同一用例及超过 512 个候选的断言通过。

以下命令均已运行通过：

```text
python tests/test_ime_core.py
python tests/test_ime_coverage.py
python tests/test_ime_pack.py
psp-gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc -c src/ime.c -o .tmp/ime-pack-test/ime.psp.o
psp-gcc -std=gnu11 -O2 -Wall -Wextra -Werror -IE:/pspdev/psp/sdk/include -Isrc -c src/ime_pack.c -o .tmp/ime-pack-test/ime_pack.psp.o
```

包测试覆盖确定性重建；错误版本、大小、记录数、CRC、字符串结尾、UTF-8、字体、排序、kind、截断、尾随；候选第 600 项窗口与选择、窗口尾边界；安装坏包保留旧 revision；打开旧 reader 后发布新包仍可读；主线程激活及缺包内置 fallback。它验证的是宿主文件系统行为和 PSP 编译，不是完整应用链接、模拟器、实机存储/性能或装卡结果。未提交代码、未安装记忆棒。

## 应用更新入口

设置ID28“更新输入法词库”，`ime_update.c`独立worker下载、流校验，主线程poll激活。
默认地址`http://melody-1320953126.cos.ap-nanjing.myqcloud.com/melody/ime/current.bin`；可在程序目录`ime-update.txt`首行写自定义HTTP地址。
服务器发布文件即生成的`tools/ime/current.bin`，不需重新编译主程序。应用只处理纯数据，没有执行下载代码。
下载有30秒无数据/10分钟总时限，联网被禁时取消。当前栈HTTP无TLS，CRC防损坏而非身份认证，不能声称有密码学来源签名。
更新成功发布新名字`ime/pack-<revision>-<time>.bin`，再原子切换`ime/active.txt`，不覆盖正在读的包；旧文件保留可手动清理。坏指针/坏包退`current.bin`再内置。
本轮仅生成服务器上传材料，未持有远端发布凭据，不能把本地生成写成远端已上传。
