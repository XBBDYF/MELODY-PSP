"""Regression for the physical replay log's reader-before-truncate EACCES.

Executes production decode_thread startup, wait_data and job_begin against local
fixture files. PSP sharing restrictions are modeled explicitly; no card access.
Removing the wait before the decoder's first open must reproduce the failure.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# Reuse the existing production-helper harness without executing its test driver.
base = (ROOT / 'tests/test_buffered_exit_replay.py').read_text(encoding='utf-8')
ns = {'__file__': str(ROOT / 'tests/test_buffered_exit_replay.py')}
exec(base[:base.index('with tempfile.TemporaryDirectory(')], ns)
function = ns['function']
audio = (ROOT / 'src/audio.c').read_text(encoding='utf-8')
dl = ns['DL']
header = ns['HEADER'].replace(
    'static int sceIoOpen(const char*p,int f,int m){(void)m;return _open(p,f|_O_BINARY,_S_IREAD|_S_IWRITE);}',
    '''static int reader_fd=-1, blocked, pending=1, scenario;
static void run_writer(void);
static int sceIoOpen(const char*p,int f,int m){
 (void)m;
 if(strstr(p,"/42.mp3") && (f&PSP_O_TRUNC) && reader_fd>=0){blocked++;return (int)0x8001000D;}
 int fd=_open(p,f|_O_BINARY,_S_IREAD|_S_IWRITE);
 if(strstr(p,"/42.mp3") && f==PSP_O_RDONLY && fd>=0)reader_fd=fd;
 return fd;
}''').replace(
    'static int sceIoClose(int f){return _close(f);}',
    'static int sceIoClose(int f){if(reader_fd==f)reader_fd=-1;return _close(f);}')
# A scheduler yield lets the real download startup run after audio startup.
header = header.replace('static void sceKernelDelayThread(int n){(void)n;}',
                        'static void sceKernelDelayThread(int n){(void)n;run_writer();}')
globals_code = r'''
typedef int SceSize;
#define AUDIO_ERROR 9
static DlJob g_stream;
static int g_gen=7,g_seek_seq,g_mv_initial_seek,g_dec_seek_seen;
static int g_quit,g_dec_quit,g_local,g_file=-1,g_state;
static long long g_file_bytes;
static char g_path[128]="cache/42.mp3",g_errmsg[96];
static int dl_stream_readable(int gen,long long off,int need){(void)gen;(void)off;(void)need;return 0;}
'''
helpers = ns['helpers']
startup = audio[audio.index('static int decode_thread('):]
startup = startup[:startup.index('\n    for (;;) {')] + '\n    return 0;\n}\n'
code = header + ns['cache_code'].replace('g_path', 'cache_paths') + ns['layout'] + helpers + globals_code
for sig in ['long long dl_stream_available(', 'int dl_stream_done(', 'int dl_stream_failed(',
            'const char *dl_stream_error(']:
    if sig.startswith('const char'):
        code += 'static const char *dl_stream_error(void){return g_stream.err;}\n'
    else:
        code += function(dl, sig)
for sig in ['static long long data_avail(', 'static int stream_finished(', 'static int wait_data(']:
    start = audio.index(sig)
    while audio[audio.index('\n', start) - 1] == ';':
        start = audio.index(sig, start + len(sig))
    end = audio.index('\n}', start) + 2
    code += audio[start:end] + '\n'
code += startup + r'''
static void run_writer(void){
 if(!pending)return;
 pending=0;
 if(scenario==1){g_dec_quit=1;return;}
 if(scenario==2){g_stream.gen=7;g_stream.error=1;strcpy(g_stream.err,"fixture write denied");return;}
 if(scenario==3)g_seek_seq++;
 Song song={0};song.id=42;
 if(job_begin(&g_stream,&song,"https://fixture/song",196608,1,7)==0){
  memset(g_wbuf[0],0x5a,DL_WBUF);g_stream.woff[0]=0;g_stream.wlen[0]=DL_WBUF;
  assert(flush_part(&g_stream,0)==0);
 }
}
int main(int argc,char**argv){
 scenario=argc>1?atoi(argv[1]):0;
 assert(_mkdir("cache")==0);
 FILE*f=fopen("cache/42.mp3","wb");assert(f);fclose(f);
 f=fopen("cache/42.part","wb");assert(f);fclose(f);
 decode_thread(0,NULL);
 /* Old startup opens the existing file without yielding; launch writer now. */
 run_writer();
 if(scenario==0 || scenario==3){
  if(blocked||g_stream.error||!g_stream.active){
   fprintf(stderr,"FAIL: reader opened before writer; O_TRUNC EACCES (%d blocked)\n",blocked);return 1;
  }
  assert(g_file>=0 && g_stream.done_bytes==DL_WBUF);
 }else if(scenario==1){assert(g_file<0 && g_dec_quit);}
 else {assert(g_file<0 && g_state==AUDIO_ERROR);assert(strstr(g_errmsg,"fixture write denied"));}
 if(g_file>=0)sceIoClose(g_file);
 if(g_stream.fd>=0)sceIoClose(g_stream.fd);
 puts("PASS: production decode startup respects writer readiness/cancel/error");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='cache-replay-open-') as temp:
    root = Path(temp)
    c, exe = root / 'fixture.c', root / 'fixture.exe'
    c.write_text(code, encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c11', '-O0', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
                    '-I', str(ROOT / 'src'), str(c), '-o', str(exe)], check=True)
    for scenario in range(4):
        directory = root / str(scenario)
        directory.mkdir()
        subprocess.run([str(exe), str(scenario)], cwd=directory, check=True)
print('LIMIT: PSP sharing modeled from physical trace; live hardware playback still needs verification')
