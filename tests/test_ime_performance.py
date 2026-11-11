"""Real pack incremental syllable commit regressions."""
from pathlib import Path
import subprocess
import shutil
R = Path(__file__).resolve().parents[1]
O = R / '.tmp/ime-performance'
O.mkdir(parents=True, exist_ok=True)
CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ime.h"
static void type(const char *s) { ime_reset(); while(*s)ime_feed(*s++); }
static int find(const char *s) { for(int i=0;i<ime_candidate_total();i++)if(!strcmp(ime_candidate(i),s))return i;return -1; }
int main(int argc,char **argv) {
 assert(argc==2); ImePack *p=NULL; char out[24];
 assert(!ime_pack_prepare(argv[1],&p)); ime_activate_pack(p);ime_set_mode(IME_PINYIN);
 type("chuyin");assert(find("初音")>=0);assert(find("初")>=0);
 assert(ime_select(find("初"),out,sizeof(out))>0);assert(!strcmp(out,"初"));assert(!strcmp(ime_preedit(),"yin"));assert(find("音")>=0);
 assert(ime_select(find("音"),out,sizeof(out))>0);assert(!ime_active());
 type("tiaotiao");assert(find("跳")>=0);assert(ime_select(find("跳"),out,sizeof(out))>0);assert(!strcmp(ime_preedit(),"tiao"));assert(find("跳")>=0);
 type("chuyin");assert(ime_select(find("初音"),out,sizeof(out))>0);assert(!ime_active());
 unsigned int io=ime_pack_io_reads(p);assert(io>0);char window[6][IME_WORD_MAX],assoc[32][IME_WORD_MAX];
 for(int repeat=0;repeat<20;repeat++) {
  type("s");assert(ime_candidate_total()>512);assert(ime_candidate_window(600,window,6)==6);
  assert(ime_assoc("初",assoc,32)>0);
 }
 assert(ime_pack_io_reads(p)==io);
 unsigned int reads=ime_pack_record_reads(p);int count=ime_assoc("初",assoc,32);
 assert(count>0);assert(ime_pack_record_reads(p)==reads);
 printf("hot path: physical read delta=%u; repeated assoc record delta=%u\n",ime_pack_io_reads(p)-io,ime_pack_record_reads(p)-reads);
 ime_suspend_dictionary();assert(!ime_dictionary_revision());assert(!ime_resume_dictionary());assert(ime_dictionary_revision());
 type("tiaotiao");assert(find("跳")>=0);
 assert(!remove(argv[1]));ime_suspend_dictionary();assert(ime_resume_dictionary()==IME_PACK_IO);assert(!ime_resume_dictionary());assert(!ime_dictionary_revision());
 puts("PASS incremental longest syllable commits");ime_activate_pack(NULL);return 0;
}
'''
(O/'test.c').write_text(CODE, encoding='utf-8')
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(R/'src'),str(O/'test.c'),str(R/'src/ime.c'),str(R/'src/ime_pack.c'),'-o',str(O/'test.exe')],check=True)
if not (R/'tools/ime/current.bin').exists():subprocess.run([__import__('sys').executable,'-B',str(R/'tools/build_ime_dict.py')],cwd=R,check=True)
shutil.copyfile(R/'tools/ime/current.bin', O/'current.bin')
subprocess.run([str(O/'test.exe'),str(O/'current.bin')],check=True)
