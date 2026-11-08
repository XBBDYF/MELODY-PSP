/*
 * MELODY PSP — MV（视频）导出
 * ------------------------------------------------------------------
 * 把已经下好的 MV（cache/mv_<mvid>_240.mp4）复制到记忆棒上用户/PC 都能直接找到
 * 的位置，供导出后取走或备份：
 *
 *   ms0:/VIDEO/MELODY-<mvid>_<歌手> - <标题>.MP4     导出结果
 *   ms0:/VIDEO/MELODY-EXPORT.TMP                     固定临时文件（复制中）
 *   ms0:/PSP/GAME/MELODY/cache/mvexport_<mvid>.txt   已导出文件名（给 mvx_exists）
 *
 * 真机 FAT 的目录枚举（sceIoDread）不可靠（docs/C2-HANDOFF.md），所以本模块
 * 从不扫目录：
 *   · 临时文件用固定名字，每次开工前先删；硬关机最多残留一个，mvx_cleanup() 开机清掉；
 *   · “是否导出过”靠每个 mvid 一个指针小文件 + 精确路径 sceIoGetstat；
 *   · 完成判定只用 sceIoGetstat / sceIoRename 的返回值，最后 sceIoSync 落盘。
 *
 * 只在 API 线程里调用（和歌曲导出 local_export 同一模式），全程 sceIo* 而不是
 * stdio，因此不需要 io_lock，也不会碰 newlib 的 FILE 链表。
 * MV 播放期间（mv_begin 会 api_shutdown）和歌曲导出期间都不要调用。
 */

#ifndef MELODY_MVEXPORT_H
#define MELODY_MVEXPORT_H

/* 导出目录与临时文件。目录可以改（例如 "ms0:/VIDEO/MELODY"），但临时文件
 * 必须和最终文件在同一个目录，rename 才只是一次目录项操作。 */
#define MVX_DIR   "ms0:/VIDEO"
#define MVX_TMP   MVX_DIR "/MELODY-EXPORT.TMP"

#define MVX_EXT        ".MP4"     /* PSP XMB 对扩展名大小写敏感，统一大写 */
#define MVX_BLOCK      32768      /* 拷贝块 = 1 个 FAT 簇（sysinfo.c 按 32KB 算容量） */
#define MVX_PCT_STEP   262144     /* 进度每 256KB 刷新一次 */
#define MVX_NAME_MAX   96         /* 文件名（不含目录）上限 */
#define MVX_PATH_MAX   128        /* 完整路径上限，和项目里 char[128] 缓冲一致 */

/* 返回值：0 成功 / -1 源不存在（没下载或只有 .part） / -2 空间不足
 *         / -3 读写或改名失败（含目标被占用） / -4 被 mvx_cancel() 取消 */
int  mvx_export(int mvid, const char *name, const char *artist);
int mvx_start(int mvid,const char *name,const char *artist);

int  mvx_pct(void);         /* 0..100，导出中按 256KB 粒度更新 */
int  mvx_active(void);      /* 1 = 正在导出 */
int  mvx_result(void);      /* 上次结果，-1 = 还没跑过 */
int  mvx_mvid(void);        /* 正在 / 上次导出的 MV id */
const char *mvx_path(void); /* 上次成功的目标路径，失败时为空串 */

/* 1 = 这个 mvid 导出过、且文件现在还在（指针文件 + sceIoGetstat，不扫目录）。
 * 文件名里带标题，无法只凭 mvid 反推，所以用指针文件记最终名字。 */
int  mvx_exists(int mvid);

void mvx_cancel(void);      /* 请求取消：拷贝循环在下一个块退出并删临时文件 */
void mvx_cleanup(void);     /* 启动时调用：无条件删掉固定临时文件（硬关机残留） */

#endif /* MELODY_MVEXPORT_H */
