# 构建说明

MELODY 是 PSP 原生程序（MIPS R4000，无 MMU），构建需要 **pspdev 工具链**。
宿主机回归测试只需要 gcc 与 Python。

---

## 1. 环境

| 组件 | 版本 / 说明 |
|---|---|
| [pspdev](https://github.com/pspdev/pspdev) | 提供 `psp-gcc`、`psp-config`、PSPSDK、libPSPGL 等；本机路径 `E:/pspdev` |
| msys2 | 提供 `bash`、`git`、`make`；本机 `D:/msys64` |
| Python 3.9+ | 跑 `tests/`、打包脚本、主题校验；`Pillow` 仅生成更新展示图时需要 |
| gcc（宿主机） | 编译测试切片，`D:/msys64/ucrt64/bin/gcc.exe` |

`psp-config --pspsdk-path` 必须指向 PSPSDK，`Makefile` 依赖它。

### 外部依赖（不在本仓库）

`Makefile` 用变量 `WILIMINI` 指向 WiliMini 根目录，默认 `E:/WiliMini`，可用 `make WILIMINI=/your/path` 覆盖：

```
CFLAGS  += -I$(WILIMINI)/decoder/include
LIBS    += tools/mvtest/build/libpspvideo_cpu.a ... $(WILIMINI)/build/ffmpeg-audio-install/lib/libav*.a
```

- `WiliMini`：软件视频解码参考实现（GPL-3.0），提供 `pspvideo` 头文件与 FFmpeg 静态库。
- `tools/mvtest/build/libpspvideo_cpu.a`：由本仓库 `tools/mvtest/` 的源码配合 WiliMini 源码构建。
- `third_party/coverjpeg_psp/build-psp/libcoverjpeg8.a`：libjpeg-turbo 的 PSP 封装，
  用 `third_party/coverjpeg_psp/prepare.py` 生成（详见该目录 README）。

如果只想编译**不依赖 MV 软件解码**的部分，可以先把 `LIBS` 里与 `pspvideo`/FFmpeg
相关的行注释掉，并把 `src/mv.c` 的软件解码分支裁掉（AVC 路径仍可用）。

---

## 2. 构建主程序

```bash
# 全量（首次或改了头文件后）
cd MELODY-PSP
export PATH="/e/pspdev/bin:$PATH"
make            # 生成 tools/prxloader/melody.prx

# 日常只改一个 .c：单模块构建脚本（更快，pspdev 的 build.mak 每次都会重链）
./build.sh src/main.o src/ui.o
```

`build.sh` 只是 `make` 的薄封装，参数是目标 `.o`。

### 构建启动器（EBOOT.PBP）

```bash
./tools/prxloader/build.sh
```

它会编译 `loader.c`（小启动器）、打包 `PARAM.SFO` 与图标、生成最终 `EBOOT.PBP`。

### 构建 MV 模块

```bash
./tools/prxloader/build_mv.sh          # 软件解码模块
./tools/prxloader/build_software_players.sh
```

模块会输出到 `.tmp/software-players/<mode>/player.prx`，打包脚本按模式复制成
`melody_mv565.prx`、`melody_mv565_fast.prx`、`melody_mv565_balanced.prx`、`melody_mv565_sharp.prx`。

---

## 3. 宿主机回归测试

测试不依赖 PSP：`tests/*.py` 把 `src/` 里的**纯函数片段**（契约头文件驱动）
抽出来，用宿主机 gcc 编译成小程序后断言行为。

```bash
python -B tests/test_dl_progress.py
python -B tests/test_net_dns_deadline_regression.py
python -B tests/test_dns_http_fallback.py
# 全跑（推荐）
python -B tests/run_all.py
# integration/ 下的用例需要额外产物，见 tests/README.md
```

注意：

- 用 `-B` 避免生成 `__pycache__`；
- 测试会把切片生成的 `.c` 放在 `.tmp/` 下，可随时删除；
- 新增功能时**先加测试再改实现**，尤其是下载进度、DNS、播放重试这类时序逻辑。

### 皮肤校验

```bash
python -B tools/validate_skin_runtime.py skin_release/melody/skins/metro
python -B tools/validate_skin_runtime.py skin_release/melody/skins/xmb
```

会检查 `skin.json`、Lua 语法、必需资源与页面函数。

---

## 4. 打包发布

```bash
python -B tools/prxloader/package_versioned_release.py 4.1.1
```

产物在 `releases/MELODY-4.1.1/`：

```
完整安装/PSP/GAME/MELODY/    小启动器 + melody.prx + MV 模块 + skins
在线更新/EBOOT.PBP           启动器 + 压缩模块（在线更新用）
在线更新/version.txt         版本 / 地址 / 字节数 / crc32 / 更新日志
manifest.json                模块尺寸与 SHA256
```

更新展示图：

```bash
python -B tools/make_update_splash.py 4.1.1
```

生成 480×272 的 `01.png`/`02.png` 与 `index.json`（revision 自动取 manifest 的 bundle crc32）。

---

## 5. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| `failure running prx/psp-build-exports.sh` | PSPSDK 的 exports 脚本权限问题，`chmod +x` 或改用 pspdev 官方 Docker |
| `undefined reference to sceNet*` | 链接顺序问题：`-lpspnet_inet -lpspnet_resolver` 必须包在 `--whole-archive` 里 |
| 启动后黑屏 | 检查 `EBOOT.PBP` 是不是小启动器、`melody.prx` 是否与固件版本匹配 |
| 皮肤不生效 | `skin_release/melody/skins/<id>/skin.json` 的 `min_app_version` 大于当前版本会被跳过 |
| 宿主机测试报 `__pycache__` | 用 `python -B` 运行 |


