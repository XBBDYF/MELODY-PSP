#include "ime_update.h"
#include "ime.h"
#include "ime_pack.h"
#include "net.h"
#include "app.h"
#include "log.h"
#include "thread_paths.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static volatile int busy,done,cancel,percent;
static SceUID worker=-1;
static ImePack *prepared;
static char prepared_name[96];
static char message[128]="尚未检查词库更新";
static int update_worker(SceSize args,void *argp){
    Http *h;SceUID file=-1;char url[768],err[128];int result=-1;long long received=0;
    (void)args;(void)argp;
    melody_bind_thread_directory();
    snprintf(url,sizeof(url),"http://melody-1320953126.cos.ap-nanjing.myqcloud.com/melody/ime/current.bin");
    io_lock();FILE *cfg=fopen(APP_DIR "ime-update.txt","rb");
    if(cfg){if(fgets(url,sizeof(url),cfg))url[strcspn(url,"\r\n")]=0;fclose(cfg);}io_unlock();
    if(strncmp(url,"http://",7)){snprintf(message,sizeof(message),"词库地址须为当前网络栈支持的HTTP地址");goto finish;}
    sceIoMkdir(APP_DIR "ime",0777);
    h=calloc(1,sizeof(*h));
    if(!h){snprintf(message,sizeof(message),"词库更新内存不足");goto finish;}
    h->sock=-1;
    if(http_open_bulk(h,url,15000,err,sizeof(err))<0){snprintf(message,sizeof(message),"词库连接失败：%.90s",err);goto cleanup;}
    if(h->content_length<IME_PACK_HEADER_SIZE||h->content_length>IME_PACK_MAX_BYTES){snprintf(message,sizeof(message),"词库文件大小无效");goto cleanup;}
    file=sceIoOpen(APP_DIR "ime/download.part",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0666);
    if(file<0){snprintf(message,sizeof(message),"无法写入词库临时文件");goto cleanup;}
    unsigned download_started=sceKernelGetSystemTimeLow(),last_data=download_started;
    while(received<h->content_length&&!cancel&&!net_offline()){
        unsigned now=sceKernelGetSystemTimeLow();
        if((unsigned)(now-last_data)>30000000u||(unsigned)(now-download_started)>600000000u){snprintf(message,sizeof(message),"词库下载超时，旧词库保留");goto cleanup;}
        unsigned char buf[4096];
        int n=http_read(h,(char*)buf,sizeof(buf),1000,&cancel,0,err,sizeof(err));
        if(n<0){if(n==-1)continue;snprintf(message,sizeof(message),"词库下载中断：%.88s",err);goto cleanup;}
        if(!n)break;
        if(received+n>IME_PACK_MAX_BYTES||sceIoWrite(file,buf,n)!=n){snprintf(message,sizeof(message),"词库写入失败");goto cleanup;}
        last_data=sceKernelGetSystemTimeLow();
        received+=n;percent=(int)(received*90/h->content_length);
        sceKernelDelayThread(1000);
    }
    if(cancel||net_offline()||received!=h->content_length){snprintf(message,sizeof(message),"词库下载已取消或长度不足");goto cleanup;}
    if(sceIoClose(file)<0){file=-1;snprintf(message,sizeof(message),"词库文件提交失败");goto cleanup;}file=-1;
    snprintf(message,sizeof(message),"正在校验词库版本、CRC与字体覆盖");percent=92;
    { ImePack *check=NULL;char path[192];
      if(ime_pack_prepare(APP_DIR "ime/download.part",&check)<0){snprintf(message,sizeof(message),"词库校验失败，原词库保留");goto cleanup;}
      unsigned revision=ime_pack_revision(check);ime_pack_close(check);
      snprintf(prepared_name,sizeof(prepared_name),"pack-%u-%u.bin",revision,(unsigned)sceKernelGetSystemTimeLow());
      snprintf(path,sizeof(path),APP_DIR "ime/%s",prepared_name);
      if(ime_pack_install(APP_DIR "ime/download.part",path)<0||ime_pack_prepare(path,&prepared)<0){snprintf(message,sizeof(message),"新词库安装失败，继续使用旧词库");goto cleanup;}
    }
    percent=100;result=0;
cleanup:
    if(file>=0)sceIoClose(file);
    http_close(h);free(h);
finish:
    if(result<0&&prepared){ime_pack_close(prepared);prepared=NULL;}
    __sync_synchronize();done=1;
    return 0;
}
int ime_update_start(void){
    if(busy||net_offline())return -1;
    cancel=done=percent=0;prepared=NULL;
    snprintf(message,sizeof(message),"正在下载独立词库");
    worker=sceKernelCreateThread("melody_ime_update",update_worker,0x1b,0x18000,PSP_THREAD_ATTR_USER,NULL);
    if(worker<0){snprintf(message,sizeof(message),"无法创建词库更新线程");return -1;}
    busy=1;
    if(sceKernelStartThread(worker,0,NULL)<0){sceKernelDeleteThread(worker);worker=-1;busy=0;return -1;}
    return 0;
}
void ime_update_poll(void){
    if(!busy||!done)return;
    SceUInt wait=0;
    if(sceKernelWaitThreadEnd(worker,&wait)<0)return;
    sceKernelDeleteThread(worker);worker=-1;__sync_synchronize();
    if(prepared){
        int fd=sceIoOpen(APP_DIR "ime/active.txt.tmp",PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0666);
        int len=(int)strlen(prepared_name),wrote=-1,closed=-1;
        if(fd>=0){wrote=sceIoWrite(fd,prepared_name,len);closed=sceIoClose(fd);}
        if(wrote==len&&closed>=0){
            sceIoRemove(APP_DIR "ime/active.txt.old");
            int had=sceIoRename(APP_DIR "ime/active.txt",APP_DIR "ime/active.txt.old")>=0;
            if(sceIoRename(APP_DIR "ime/active.txt.tmp",APP_DIR "ime/active.txt")<0){
                if(had)sceIoRename(APP_DIR "ime/active.txt.old",APP_DIR "ime/active.txt");
                ime_pack_close(prepared);prepared=NULL;
                snprintf(message,sizeof(message),"词库索引保存失败，旧词库保留");busy=done=0;return;
            }
        }else{
            ime_pack_close(prepared);prepared=NULL;snprintf(message,sizeof(message),"词库索引写入失败，旧词库保留");busy=done=0;return;
        }
        ime_activate_pack(prepared);prepared=NULL;
        g_app.ime_sel=g_app.ime_off=g_app.chip_sel=g_app.chip_off=g_app.assoc_n=0;
        snprintf(message,sizeof(message),"词库已热更新，版本%u",ime_dictionary_revision());
    }
    busy=0;done=0;mlog("ime update: %s",message);
}
void ime_update_cancel(void){cancel=1;}
int ime_update_busy(void){return busy;}
int ime_update_percent(void){return percent;}
const char *ime_update_message(void){return message;}
