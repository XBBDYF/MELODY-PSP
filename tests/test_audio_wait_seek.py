"""A seek interrupt must apply the new target, not exit the decoder worker."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/audio-wait-seek';O.mkdir(exist_ok=True)
s=(R/'src/audio.c').read_text(encoding='utf-8');a=s.index('if(r==-1){if(!g_quit');b=s.index('\n',a);branch=s[a:b]
code='''#include <assert.h>\n#include <stdio.h>\nint main(void){int g_quit=0,g_dec_quit=0,g_gen=7,my=7,g_seek_seq=2,local_seek=1,r=-1,applied=0;for(int i=0;i<3;++i){if(i==1){local_seek=g_seek_seq;applied=1;r=1;}\n'''+branch+'''\n}assert(applied&&local_seek==2);g_gen=8;applied=0;r=-1;for(int i=0;i<3;++i){\n'''+branch+'''\napplied=1;}assert(!applied);puts("PASS production wait-abort branch: new seek keeps decoder alive, changed track retires old decoder");return 0;}\n'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
