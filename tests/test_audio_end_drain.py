"""Decoder EOF must not announce END while decoded PCM remains to be heard."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];s=(R/'src/audio.c').read_text(encoding='utf-8');a=s.index('                if (stream_finished(my) && at_end) {');b=s.index('\n                sceKernelDelayThread',a);body=s[a:b]
pre=r'''
#include <assert.h>
#include <stdio.h>
#define AUDIO_END 4
static int g_dec_done,state=2,flushed;
static void stage_flush(void){flushed=1;}
static int stream_finished(int g){(void)g;return 1;}
static void set_state(int st,const char*reason){(void)reason;state=st;}
int main(void){int my=1,at_end=1;for(int once=0;once<1;++once){
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'t.c').write_text(pre+body+'}\nassert(flushed&&g_dec_done&&state==2);puts("PASS decoder EOF only marks done; output drain owns END");}\n',encoding='utf-8');subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Wno-unused-function',str(p/'t.c'),'-o',str(p/'t.exe')],check=True);subprocess.run([str(p/'t.exe')],check=True)
