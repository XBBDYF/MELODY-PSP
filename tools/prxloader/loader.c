#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../../src/mv_settings_contract.h"
#include "../../src/app.h"

#include "melody_host.h"
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
static int bundle_progress_shown;
static void bundle_progress(const char*stage,const char*name,unsigned percent){
    static unsigned last_time,last_percent=101;static const char*last_stage;
    unsigned now=sceKernelGetSystemTimeLow();if(percent>100)percent=100;
    if(bundle_progress_shown&&last_stage==stage&&percent==last_percent&&now-last_time<100000u)return;
    if(bundle_progress_shown&&last_stage==stage&&percent<100&&now-last_time<50000u)return;
    if(!bundle_progress_shown){pspDebugScreenInit();pspDebugScreenSetBackColor(0xff181514);pspDebugScreenSetTextColor(0xfff4f4f4);pspDebugScreenClear();bundle_progress_shown=1;}
    last_time=now;last_percent=percent;last_stage=stage;
    pspDebugScreenSetXY(5,5);pspDebugScreenPrintf("MELODY - Installing update                         ");
    pspDebugScreenSetXY(5,9);pspDebugScreenPrintf("%-52.52s",stage);
    pspDebugScreenSetXY(5,12);pspDebugScreenPrintf("%-52.52s",name);
    pspDebugScreenSetXY(5,16);pspDebugScreenPrintf("Overall: %3u%%                                      ",percent);
    pspDebugScreenSetXY(5,26);pspDebugScreenPrintf("Keep power on. Do not remove the memory stick.");
    volatile unsigned*ram=(volatile unsigned*)((unsigned)sceGeEdramGetAddr()|0x40000000u);
    for(unsigned y=153;y<163;++y)for(unsigned x=40;x<440;++x)ram[y*512+x]=x<40+4*percent?0xff4240e0u:0xff39332eu;
    __asm__ volatile("sync":::"memory");
}
#define MB_PROGRESS(stage,name,percent) bundle_progress(stage,name,percent)
#include "bundle_update.h"

PSP_MODULE_INFO("MELODY Loader", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(256);

/* 和 tools/prxloader/mvprx.c 里的定义必须一致（同一份 ABI）。
 * magic 用来判定对面 PRX 是不是带 release_heap 的新版本：
 * 老 PRX 里这个位置是别的静态数据，绝不能盲调。 */
#define MV_API_MAGIC 0x4D563537u   /* 'MV57': explicit unsafe-stop result contract */

typedef struct {
    int (*play)(const PspVideoOptions *opt, PspVideoResult *res);
    void (*stop)(void);
    unsigned magic;
    void (*release_heap)(void);
} MvApi;
#define NATIVE_AVC_API_MAGIC 0x4D563538u
typedef struct {MvApi base;int (*heap_release_status)(void);} NativeMvApi;

static SceUID g_mv_mod = -1;
static MvApi *g_mv_api;
static SceUID g_req = -1;
static SceUID g_done = -1;
static volatile int g_action;
static volatile int g_result = -1;
/* 模块还挂着但已经不可用（stop 成功、unload 失败；或曾经崩溃过）：
 * 绝不能再 sceKernelLoadModule 第二份 —— 那会再吃掉 4.7MB + 8MB，真机必挂。 */
static int g_mv_retired;

/* ME 内核桥（me_bridge.prx）：整个 app 生命周期只 load/start 一次，永不卸载。
 * 它是内核模块（attr 0x1006 = KERNEL|SINGLE_LOAD|SINGLE_START），卸载/重载
 * 都可能把机器带走；起不来就永远留在 CPU 路径上，不影响音乐和 CPU 版 MV。 */
static SceUID g_me_mod = -1;
static int g_me_tried;
static int g_me_ok;
/* ME 版 MV PRX 一次开机只允许用一次（V1 安全模式）。
 * 原因：me_bridge 的 ready 只有在 meLabShutdown 成功后才清 0；而 MV PRX 每轮
 * 播完都会被卸载，me_task 所在镜像被释放、下轮重新加载到不同地址。若 ready
 * 残留为 1，第二次 meLabInit 会直接返回而不重新注入入口 -> ME 跳到已释放的
 * 代码 -> 整机硬死机（2026-11-01 11:16 真机）。在 bridge 侧硬化之前，第二次
 * 及以后的 MV 一律走 CPU 版（psp_video_play 仍安全、只是 CPU 渲染）。 */
static int g_mv_me_used;
static int g_mv_avc;

static int free_kb(void) { return (int)(sceKernelTotalFreeMemSize() / 1024); }

static void log_host(const char *s)
{
    int fd = sceIoOpen("./loader.txt",
                       PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);

    if (fd >= 0) {
        sceIoWrite(fd, s, strlen(s));
        sceIoClose(fd);
        sceIoSync("ms0:", 0);   /* 崩溃/重启前这几行必须落卡，行数很少 */
    }
}

/* 文件存在即返回 1（不看内容）。 */
static int file_exists(const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);

    if (fd < 0) return 0;
    sceIoClose(fd);
    return 1;
}

/* 加载 me_bridge.prx：0 = 可用，<0 = 不可用（继续走 CPU 路径）。
 * 只做一次，失败不重试；失败也不 unload —— 内核模块 stop/unload 状态未知，
 * 留着（8.5KB）比卸掉安全。用户态 EBOOT 加载内核 PRX 需要 CFW：
 * OFW/HEN 上 expect 0x80020148 (UNSUPPORTED_PRX_TYPE)，此时只记日志，
 * ME 版 MV PRX 起不来，CPU 版照旧。 */
static int load_me_bridge_once(void)
{
    int status = 0;
    int sr;
    char b[160];

    if (g_me_tried) return g_me_ok ? 0 : -1;
    g_me_tried = 1;
    g_me_mod = sceKernelLoadModule("./me_bridge.prx", 0, NULL);
    if (g_me_mod < 0) {
        snprintf(b, sizeof(b), "me_bridge load uid=%d err=0x%08X free=%dK\n",
                 g_me_mod, (unsigned)g_me_mod, free_kb());
        log_host(b);
        return -1;
    }
    sr = sceKernelStartModule(g_me_mod, 0, NULL, &status, NULL);
    snprintf(b, sizeof(b), "me_bridge load uid=%d start=%d status=%d free=%dK\n",
             g_me_mod, sr, status, free_kb());
    log_host(b);
    if (sr < 0) {
        snprintf(b, sizeof(b), "me_bridge start failed 0x%08X, keep cpu path\n",
                 (unsigned)sr);
        log_host(b);
        return -1;
    }
    g_me_ok = 1;
    return 0;
}

#include "native_avc_host.h"

/* Runs on the loader's main thread: module manager calls from a PRX thread
 * deadlock in PPSSPP HLE, so the PRX-side host API forwards work here. */
static int do_mv_load(void)
{
    MvApi *api = NULL;
    MvApi **pp = &api;
    int status = 0;
    int r, sr;
    char b[160];

    if (g_mv_retired) return -3;
    if (g_mv_api) return 0;
    if (g_mv_retired || g_mv_mod >= 0) {
        log_host("mv load refused: retired/stale\n");
        return -3;
    }
    /* MV PRX 选择。me_bridge 必须在 MV PRX load+start 之前装好（ME 版静态
     * 导入 me_lab）；默认选择保持原样：mvfast.txt 是 '1' 走 fast，否则 normal。
     * A/B 开关：./mvme.txt 存在才考虑 ME 版；内核桥起不来
     * 时不能硬上（me_lab 导入解析不了 → MV 整个播不了），回退 CPU。 */
    {
        int fast = 0;
        int soft_quality=0;
        int me = 0;
        const char *mv_path;

        /* 2026-11-01 起：CC 桥（Custom Core）的每次会话都是
         * meLabInit → 播放 → meLabShutdown 的受控 start/stop，
         * 且 shutdown 成功后会清 ready，所以允许"每支 MV 都用 ME"。
         * （MIST 桥时代限制为一次开机一次，是因为它二次 reset ME 会硬死机。） */
        int native_requested = 0;
        int nf = sceIoOpen("./mvavc.txt", PSP_O_RDONLY, 0);
        if (nf >= 0) { char c=0; native_requested=sceIoRead(nf,&c,1)==1&&c=='1'; sceIoClose(nf); }
        int explicit_decoder=0;
        nf=sceIoOpen("./" MV_DECODER_FILE,PSP_O_RDONLY,0);
        if(nf>=0){char value[16]={0};int n=sceIoRead(nf,value,sizeof(value)-1);sceIoClose(nf);if(n>0){native_requested=mv_decoder_value(atoi(value));explicit_decoder=1;}}
        if(native_requested==MV_DECODE_AUTO){
            native_requested=0;nf=sceIoOpen("./mvsession.txt",PSP_O_RDONLY,0);
            if(nf>=0){char value=0;if(sceIoRead(nf,&value,1)==1)native_requested=value=='1';sceIoClose(nf);}
        }
        g_mv_avc = 0;
        if (native_requested) {
            if (load_native_avc_once() < 0) { log_host("native AVC setup failed; no custom ME fallback in this boot\n"); return -6; }
            g_mv_avc = 1;
            mv_path = "./melody_mv_avc.prx";
        }
        if (!explicit_decoder && !native_requested && !native_avc_tried && file_exists("./mvme.txt")) {
            if (load_me_bridge_once() == 0) {
                me = 1;
                g_mv_me_used = 1;   /* 仅作记录/日志用，不再限制次数 */
                mv_path = "./melody_mv565_me.prx";
            } else {
                log_host("mv me requested but me_bridge down, fallback cpu\n");
            }
        }
        if (!me && !g_mv_avc) {
            int fd = sceIoOpen("./mvfast.txt", PSP_O_RDONLY, 0);

            if (fd >= 0) {
                char c = 0;

                if (sceIoRead(fd, &c, 1) == 1 && c == '1') fast = 1;
                sceIoClose(fd);
            }
            fd=sceIoOpen("./" MV_SOFT_QUALITY_FILE,PSP_O_RDONLY,0);
            if(fd>=0){char value[16]={0};int n=sceIoRead(fd,value,sizeof(value)-1);sceIoClose(fd);if(n>0){soft_quality=atoi(value);fast=soft_quality==1;}}
            mv_path = soft_quality==2?"./melody_mv565_balanced.prx":soft_quality==3?"./melody_mv565_sharp.prx":fast ? "./melody_mv565_fast.prx"
                           : "./melody_mv565.prx";
        }
        g_mv_mod = sceKernelLoadModule(mv_path, 0, NULL);
        snprintf(b, sizeof(b), "mv load me=%d fast=%d mod=%08X free=%dK\n",
                 me, fast, g_mv_mod, free_kb());
        log_host(b);

        if (g_mv_mod < 0 && me) {
            /* ME PRX 装不起来（导入解析/内存/固件不匹配）：退回 CPU 版，
             * 别让 MV 因为一个实验开关直接不可用。 */
            log_host("mv me prx load failed, fallback cpu\n");
            me = 0;
            g_mv_mod = sceKernelLoadModule("./melody_mv565.prx", 0, NULL);
            snprintf(b, sizeof(b), "mv load cpu fallback mod=%08X free=%dK\n",
                     g_mv_mod, free_kb());
            log_host(b);
        }
    }
    if (g_mv_mod < 0) {
        snprintf(b, sizeof(b), "mv load failed 0x%08X\n", g_mv_mod);
        log_host(b);
        return -1;
    }
    r = sceKernelStartModule(g_mv_mod, sizeof(pp), &pp, &status, NULL);
    snprintf(b, sizeof(b), "mv mod=%d start=%d status=%d api=%08X free=%dK\n",
             g_mv_mod, r, status, (unsigned)api, free_kb());
    log_host(b);
    if (r < 0 || status != 0 || !api || api->magic != (g_mv_avc?NATIVE_AVC_API_MAGIC:MV_API_MAGIC)) {
        status=-1;
        sr = sceKernelStopModule(g_mv_mod, 0, NULL, &status, NULL);
        snprintf(b, sizeof(b), "mv start failed, stop=%08X\n", sr);
        log_host(b);
        if (sr < 0 || status != 0) { g_mv_retired=1;g_mv_api=NULL;return -1; }
        sr=sceKernelUnloadModule(g_mv_mod);
        if(sr<0){g_mv_retired=1;g_mv_api=NULL;return -1;}
        g_mv_mod = -1;
        return -1;
    }
    g_mv_api = api;
    return 0;
}

static int do_mv_unload(void)
{
    int status = 0;
    int r1, r2;
    int before, after;
    char b[160];

    if (g_mv_mod < 0) return g_mv_retired ? -1 : 0;
    before = free_kb();
    if (g_mv_api) g_mv_api->stop();
    if (g_mv_avc) {
        /* StopModule unlinks imports. Execute gated callbacks while Started. */
        snprintf(b,sizeof(b),"native AVC before release_heap api=%08X hook=%08X free=%dK\n",(unsigned)g_mv_api,(unsigned)(g_mv_api?g_mv_api->release_heap:NULL),free_kb());log_host(b);
        if(g_mv_api&&g_mv_api->release_heap)g_mv_api->release_heap();
        snprintf(b,sizeof(b),"native AVC after release_heap free=%dK before unload\n",free_kb());log_host(b);
        NativeMvApi*native=(NativeMvApi*)g_mv_api;
        int heap_status=(native&&native->heap_release_status)?native->heap_release_status():-1;
        snprintf(b,sizeof(b),"native AVC heap_release_status=%08X\n",(unsigned)heap_status);log_host(b);
        if(heap_status<0){g_mv_retired=1;g_mv_api=NULL;return heap_status;}
        status=-1;
        r1 = sceKernelStopModule(g_mv_mod, 0, NULL, &status, NULL);
        snprintf(b,sizeof(b),"native AVC retire stop=%08X status=%08X\n",(unsigned)r1,(unsigned)status);log_host(b);
        if(r1<0||status!=0){g_mv_retired=1;g_mv_api=NULL;return r1<0?r1:-1;}
        r2=sceKernelUnloadModule(g_mv_mod);
        snprintf(b,sizeof(b),"native AVC unload=%08X free=%dK\n",(unsigned)r2,free_kb());log_host(b);
        if(r2<0){g_mv_retired=1;g_mv_api=NULL;return r2;}
        g_mv_mod=-1;g_mv_api=NULL;g_mv_avc=0;return 0;
    }

    /* 关键：先让 PRX 把 _sbrk 那 8MB 用户分区还给系统（module_stop 不会被
     * 内核调用，所以只能由这里显式调）。老 PRX 没有这个钩子，靠 magic 判断。 */
    if (g_mv_api && g_mv_api->magic == MV_API_MAGIC && g_mv_api->release_heap) {
        g_mv_api->release_heap();
        log_host("mv unload: release_heap called\n");
    } else {
        log_host("mv unload: PRX has no release_heap (old build) -> 8MB will leak\n");
    }
    r1 = sceKernelStopModule(g_mv_mod, 0, NULL, &status, NULL);
    r2 = (r1>=0 && status==0) ? sceKernelUnloadModule(g_mv_mod) : -1;
    after = free_kb();
    snprintf(b, sizeof(b), "mv unload stop=%08X unload=%08X free=%dK->%dK (+%dK)\n",
             r1, r2, before, after, after - before);
    log_host(b);
    if (r2 < 0 || r1 < 0 || status != 0) {
        /* 模块停不掉/卸不掉：它已经不能再跑（也可能是 stopped 状态），
         * 记成 retired，永远不再 load 第二份。 */
        log_host("mv unload failed, module retired (no reload)\n");
        g_mv_retired = 1;
        g_mv_api = NULL;
        return r1<0?r1:r2<0?r2:-1;
    }
    /* 镜像 ~4.7MB + sbrk 堆 8MB ≈ 12.9MB；只回来一半就说明堆没还成，
     * 下一次起播会被 mv.c 的内存闸门挡掉（不会再重启）。 */
    if (after - before < 11 * 1024)
        log_host("mv unload: heap NOT fully reclaimed (8MB sbrk block leaked)\n");
    g_mv_mod = -1;
    g_mv_api = NULL;
    return 0;
}

/* 握手等 8 秒就放弃（以前是 NULL = 永远等）。loader 主线程卡在
 * sceKernelStopModule 里时（PPSSPP 会等模块自己的线程退出，PRX 的线程
 * 不退就永远不回来），主线程会在这句 API 上永久冻结 —— 界面就停在
 * 「菜单 + 播放 MV」那一帧。宁可让这次 MV 起播失败（UI 弹提示），
 * 也不能把主线程钉死在这里。 */
#define MV_HANDSHAKE_TIMEOUT_US  (8 * 1000 * 1000)

static int wait_done(void)
{
    SceUInt tmo = MV_HANDSHAKE_TIMEOUT_US;

    if (sceKernelWaitSema(g_done, 1, &tmo) >= 0) return 0;
    {
        /* 迟到的 g_done 要吃掉，否则下一次握手会「假成功」 */
        SceUInt zero = 0;

        sceKernelWaitSema(g_done, 1, &zero);
    }
    log_host("host: loader handshake timeout (8s), module retired\n");
    g_mv_retired = 1;      /* 状态未知：绝不再 load 第二份（会再吃 12MB） */
    return -5;
}

static int host_mv_load(void)
{
    char b[96];

    if (g_mv_retired) return -3;
    if (g_mv_api) return 0;
    if (g_mv_retired) {
        /* 上一次卸载只成功了一半：宁可让 MV 起播失败（UI 弹提示），
         * 也不能再 load 第二份模块把内存再吃掉 12MB。 */
        log_host("mv load refused: retired (module unusable)\n");
        return -3;
    }
    if (g_mv_mod >= 0) {
        /* 模块还在但 api 丢了：只有整块内存还回来才敢重装 */
        log_host("mv load refused: stale module still loaded\n");
        return -4;
    }
    if (g_req < 0 || g_done < 0) return -1;
    log_host("host load request\n");
    g_action = 1;
    g_result = -1;
    sceKernelSignalSema(g_req, 1);
    if (wait_done() < 0) return -5;
    snprintf(b, sizeof(b), "host load done result=%d api=%08X\n",
             g_result, (unsigned)g_mv_api);
    log_host(b);
    return g_result;
}

static int host_mv_play(const PspVideoOptions *opt, PspVideoResult *res)
{
    int r = host_mv_load();
    int pr;

    if (r < 0 || !g_mv_api) {
        log_host("host play: no api\n");
        if (res) res->error = -1;
        return -1;
    }
    /* 这两行是「崩在 PRX 里面还是外面」的唯一判据：设备重启后如果只有
     * enter 没有 returned，说明 psp_video_play 没回来。 */
    {
        char b[96];

        snprintf(b, sizeof(b), "host play: enter free=%dK max=%dK\n",
                 free_kb(), (int)(sceKernelMaxFreeMemSize() / 1024));
        log_host(b);
    }
    pr = g_mv_api->play(opt, res);
    {
        char b[96];

        snprintf(b, sizeof(b), "host play: returned %d free=%dK max=%dK\n", pr,
                 free_kb(), (int)(sceKernelMaxFreeMemSize() / 1024));
        log_host(b);
    }
    return pr;
}

static void host_mv_stop(void)
{
    if (g_mv_api) g_mv_api->stop();
}

static int host_mv_unload(void)
{
    if (g_mv_mod < 0) return g_mv_retired ? -1 : 0;
    if (g_req < 0 || g_done < 0) return -1;
    g_action = 2;
    int signal=sceKernelSignalSema(g_req, 1);
    if(signal<0){g_mv_retired=1;return signal;}
    if (wait_done() < 0) {
        g_mv_api = NULL;
        return -1;
    }
    return g_result;
}

static MelodyHost g_host_api = {
    host_mv_load, host_mv_play, host_mv_stop, host_mv_unload
};

static int launch_directory(int argc,char**argv){
 char dir[512];if(argc<1||!argv||!argv[0])return 0;
 const char*slash=strrchr(argv[0],'/');if(!slash)return 0;
 size_t n=(size_t)(slash-argv[0]+1);if(n>=sizeof(dir))return -1;
 memcpy(dir,argv[0],n);dir[n]=0;
 if(sceIoChdir(dir)<0)return -1;
 return chdir(dir);
}
int main(int argc,char**argv)
{
    if(launch_directory(argc,argv)<0){sceKernelExitGame();return 0;}

    MelodyHost *hp = &g_host_api;
    SceUID mod;
    int status = -1;
    int r;
    int fd;
    char b[128];
    int n;

    g_req = sceKernelCreateSema("mv_req", 0, 0, 1, NULL);
    g_done = sceKernelCreateSema("mv_done", 0, 0, 1, NULL);

    int bundle=mb_install();
    if(bundle<0){bundle_progress("Update failed - previous program retained","Check free space and restart to retry",0);log_host("bundle update failed; previous modules retained; retry after checking card space\n");sceKernelDelayThread(3000000);sceKernelExitGame();return 0;}
    if(bundle>0){
        unsigned revision=0;int mark=sceIoOpen(MB_MARK,PSP_O_RDONLY,0);if(mark>=0){sceIoRead(mark,&revision,sizeof(revision));sceIoClose(mark);}
        char preview[80];int size=snprintf(preview,sizeof(preview),APP_VER "\n%08x\n",revision);int pf=sceIoOpen("./update-preview-pending.txt",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0600);if(pf>=0){sceIoWrite(pf,preview,size);sceIoClose(pf);}
    }
    mod = sceKernelLoadModule("./melody.prx", 0, NULL);
    if (mod < 0) {
        fd = sceIoOpen("./loader.txt",
                       PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        n = snprintf(b, sizeof(b), "load failed 0x%08X\n", mod);
        if (fd >= 0) { sceIoWrite(fd, b, n); sceIoClose(fd); }
        sceKernelDelayThread(3 * 1000 * 1000);
        sceKernelExitGame();
        return 0;
    }
    struct{MelodyHost*host;char root[512];}launch={0};launch.host=hp;
    if(!getcwd(launch.root,sizeof(launch.root))){sceKernelExitGame();return 0;}
    r = sceKernelStartModule(mod, sizeof(launch), &launch, &status, NULL);
    fd = sceIoOpen("./loader.txt",
                   PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    n = snprintf(b, sizeof(b), "mod=%d start=%d status=%d\n", mod, r, status);
    if (fd >= 0) { sceIoWrite(fd, b, n); sceIoClose(fd); }

    for (;;) {
        sceKernelWaitSema(g_req, 1, NULL);
        if (g_action == 1) g_result = do_mv_load();
        else { g_result = do_mv_unload(); }
        sceKernelSignalSema(g_done, 1);
    }
    return 0;
}
