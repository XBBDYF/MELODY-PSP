"""Regression: common characters survive generation and reach the Lua model."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1]; O=R/'.tmp/ime-coverage';O.mkdir(parents=True,exist_ok=True)
code=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ime.h"
static void type(const char*s){ime_reset();while(*s)ime_feed(*s++);}
static int has(const char*s){for(int i=0;i<ime_candidate_count();++i)if(!strcmp(s,ime_candidate(i)))return 1;return 0;}
int main(void){
 ime_set_mode(IME_PINYIN);
 type("zhu");assert(has("祝"));assert(has("朱"));assert(has("竹"));assert(ime_candidate_count()>15);
 int zhu_rank=0;while(zhu_rank<ime_candidate_count()&&strcmp(ime_candidate(zhu_rank),"祝"))++zhu_rank;assert(zhu_rank<8);
 type("chen");assert(has("陈"));assert(has("尘"));assert(has("晨"));
 type("shengrikuaile");assert(has("生日快乐"));
 type("zhufu");assert(has("祝福"));
 char out[32][IME_WORD_MAX];int n=ime_assoc("祝",out,32),found=0;
 for(int i=0;i<n;++i)if(!strcmp(out[i],"祝福"))found=1;
 assert(found);
 puts("PASS IME coverage: zhu/祝, >15 characters, phrase input and local association");
}
'''
(O/'test.c').write_text(code,encoding='utf-8')
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-I'+str(R/'src'),str(O/'test.c'),str(R/'src/ime.c'),'-o',str(O/'test.exe')],check=True)
subprocess.run([str(O/'test.exe')],check=True)
s=(R/'src/skin_runtime.c').read_text(encoding='utf-8')
assert 'int n=ime_candidate_count();if(n>6)' not in s, 'Theme model must not truncate the candidate list'
