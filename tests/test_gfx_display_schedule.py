"""Execute production gfx_flip against delayed-vsync PSP display semantics."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/gfx.c").read_text(encoding="utf-8")
START = SOURCE.index("void gfx_flip(void)")
END = SOURCE.index("void gfx_set_immediate", START)
FLIP = SOURCE[START:END]
PRELUDE = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GFX_RENDERER_CPU 0
#define GFX_RENDERER_GE 1
#define PSP_DISPLAY_SETBUF_IMMEDIATE 0
#define PSP_DISPLAY_SETBUF_NEXTFRAME 1
#define PSP_DISPLAY_PIXEL_FORMAT_565 0
#define FB_STRIDE 512
#define SCR_H 272
typedef struct { unsigned draws, uploads, flushes; } GfxGuStats;
static unsigned char ram[2][512*272*2], vram[2][512*272*2];
static void *g_ram[]={ram[0],ram[1]}, *g_vram[]={vram[0],vram[1]};
static void *g_fb_display[]={vram[0],vram[1]};
static int g_back,g_last,g_ready=1,g_direct_video,g_skip_copy,g_immediate;
static int g_renderer,g_ge_frame;
static unsigned g_render_started,g_render_frames,g_render_time_total;
static unsigned g_render_draws,g_render_uploads,g_render_syncs;
static void *scanout=vram[1], *pending;
static int synced, in_vblank, get_calls, mode=-1, fail_set, bad_get, mismatches;
static unsigned sceKernelGetSystemTimeLow(void){return 1;}
static void gfx_gu_end(void){synced=1;}
static void gfx_gu_get_stats(GfxGuStats *s){memset(s,0,sizeof(*s));}
static void sceKernelDcacheWritebackRange(void *p,int n){(void)p;(void)n;}
static void sceDmacMemcpy(void *a,void *b,int n){memcpy(a,b,n);}
static int sceDisplayWaitVblankStart(void){
    if(g_renderer==GFX_RENDERER_GE)assert(synced);
    if(pending){scanout=pending;pending=0;}in_vblank=1;return 0;
}
static int sceDisplaySetFrameBuf(void *p,int w,int f,int sync){
    assert(in_vblank);assert(w==512&&f==0);mode=sync;
    if(fail_set)return -1;
    if(sync==PSP_DISPLAY_SETBUF_NEXTFRAME)pending=p;
    else {scanout=p;pending=0;}
    return 0;
}
static int sceDisplayGetFrameBuf(void **p,int *w,int *f,int sync){
    assert(sync==PSP_DISPLAY_SETBUF_IMMEDIATE);get_calls++;
    *p=bad_get?vram[1]:scanout;*w=512;*f=0;return 0;
}
static void mlog(const char *fmt,...){
    char text[512];va_list args;va_start(args,fmt);
    vsnprintf(text,sizeof(text),fmt,args);va_end(args);
    if(strstr(text,"mismatch"))mismatches++;
}
'''
CHECKS = r'''
int main(int argc,char **argv){
    assert(argc==2);
    if(!strcmp(argv[1],"ge")){
        g_renderer=GFX_RENDERER_GE;
        for(int i=0;i<241;i++){
            /* GE starts writing immediately after previous flip, before vblank. */
            assert(g_vram[g_back]!=scanout);
            g_ge_frame=1;synced=0;in_vblank=0;gfx_flip();
            assert(g_vram[g_back]!=scanout);
            assert(mode==PSP_DISPLAY_SETBUF_IMMEDIATE);
        }
        assert(get_calls==3);assert(!mismatches);
    }else if(!strcmp(argv[1],"cpu")){
        ram[0][0]=77;gfx_flip();
        assert(mode==PSP_DISPLAY_SETBUF_NEXTFRAME);
        assert(scanout==vram[1]&&pending==vram[0]);
        assert(vram[0][0]==77&&get_calls==0);
    }else if(!strcmp(argv[1],"immediate")){
        g_immediate=1;gfx_flip();
        assert(mode==PSP_DISPLAY_SETBUF_IMMEDIATE&&scanout==vram[0]);
        assert(get_calls==0);
    }else if(!strcmp(argv[1],"mismatch")){
        g_renderer=GFX_RENDERER_GE;g_ge_frame=1;bad_get=1;gfx_flip();
        assert(get_calls==1&&mismatches==1);
    }else if(!strcmp(argv[1],"set-error")){
        g_renderer=GFX_RENDERER_GE;g_ge_frame=1;fail_set=1;gfx_flip();
        assert(get_calls==1&&mismatches==1);
    }else assert(0);
    puts("PASS actual gfx_flip display scheduling");return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix="gfx-display-") as tmp:
        source = Path(tmp) / "test.c"
        exe = Path(tmp) / "test.exe"
        source.write_text(PRELUDE + FLIP + CHECKS, encoding="utf-8")
        subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c99", "-O2", "-Wall", "-Wextra", str(source), "-o", str(exe)], check=True)
        for case in ("ge", "cpu", "immediate", "mismatch", "set-error"):
            subprocess.run([str(exe), case], check=True)

if __name__ == "__main__":
    main()
