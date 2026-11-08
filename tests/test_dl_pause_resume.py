"""Run production queue pause/resume and persistence with isolated host I/O."""
from pathlib import Path
import ast
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# Reuse platform boundaries without executing the other regression suite.
module = ast.parse((ROOT / 'tests/test_dl_background.py').read_text(encoding='utf-8'))
preamble = next(ast.literal_eval(n.value) for n in module.body
                if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'preamble' for t in n.targets))
preamble = preamble.replace('static int cache_has(long long id){(void)id;return 0;}',
                            'static long long cached_id; static int cache_has(long long id){return id==cached_id;}')
source = (ROOT / 'src/dl.c').read_text(encoding='utf-8')
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
# The progress contract is header-only and must travel with the extracted source.
source = '#include "dl_progress_contract.h"\n' + source
tail = r'''
static Song song(long long id){Song s;memset(&s,0,sizeof(s));s.id=id;return s;}
static void enqueue(long long id){Song s=song(id);assert(dl_enqueue(&s,"queue",900000)>=0);}
static void begin_head(void){assert(job_begin(&g_qjob,&g_queue[0].song,g_urls[0],g_queue[0].total,0,0)==0);g_queue[0].state=DL_ACTIVE;}
int main(int argc,char**argv){assert(argc==2);job_reset(&g_stream);job_reset(&g_qjob);
assert(DL_PENDING==0&&DL_ACTIVE==1&&DL_DONE==2&&DL_FAILED==3&&DL_PAUSED==4);
if(!strcmp(argv[1],"pending")){enqueue(1);g_queue[0].pct=37;int epoch=g_io_epoch;assert(dl_queue_pause(0)==0);assert(g_queue[0].state==DL_PAUSED);assert(g_queue[0].pct==37);assert(g_qjob.fd<0);assert(g_io_epoch==epoch);assert(dl_queue_pause(0)==-1);assert(dl_queue_resume(0)==0);assert(g_queue[0].state==DL_PENDING);assert(dl_queue_resume(0)==-1);assert(dl_queue_pause(-1)==-1);assert(dl_queue_resume(32)==-1);}
else if(!strcmp(argv[1],"active")){enqueue(1);begin_head();g_qjob.woff[0]=0;g_qjob.wlen[0]=4096;memset(g_wbuf[0],0x5a,4096);int fd=g_qjob.fd;int epoch=g_io_epoch;assert(dl_queue_pause(0)==0);assert(g_qjob.fd==fd&&g_qjob.wlen[0]==4096);assert(g_queue[0].state==DL_ACTIVE);dl_thread(0,NULL);assert(g_queue_n==1&&g_queue[0].state==DL_PAUSED);assert(g_qjob.fd<0&&!g_qjob.active);assert(g_io_epoch==epoch);assert(g_queue[0].pct==0);assert(dl_queue_resume(0)==0);begin_head();assert(job_prefix(&g_qjob)==4096);assert(g_qjob.done_bytes==4096);assert(sceIoLseek(g_qjob.fd,0,PSP_SEEK_END)==4096);}
else if(!strcmp(argv[1],"skip")){enqueue(1);enqueue(2);assert(dl_queue_pause(0)==0);dl_thread(0,NULL);assert(queue_reads>0);assert(g_queue[0].id==2);assert(g_queue[1].id==1&&g_queue[1].state==DL_PAUSED);}
else if(!strcmp(argv[1],"allpaused")){enqueue(1);enqueue(2);assert(dl_queue_pause(0)==0);assert(dl_queue_pause(1)==0);dl_thread(0,NULL);assert(!opens&&!queue_reads);assert(g_queue_n==2&&g_queue[0].id==1);}
else if(!strcmp(argv[1],"target")){enqueue(1);enqueue(2);begin_head();assert(dl_queue_pause(0)==0);Song s=song(1);start_stream(&s,"stream",900000,7);dl_thread(0,NULL);assert(g_queue_n==1&&g_queue[0].id==2);assert(g_queue[0].state!=DL_PAUSED);assert(g_stream.id==1&&g_stream.gen==7);}
else if(!strcmp(argv[1],"pausedplay")){enqueue(1);begin_head();g_qjob.woff[0]=0;g_qjob.wlen[0]=4096;memset(g_wbuf[0],0x5a,4096);assert(dl_queue_pause(0)==0);dl_thread(0,NULL);assert(g_queue_n==1&&g_queue[0].state==DL_PAUSED);assert(!g_qjob.active&&g_qjob.fd<0);Song s=song(1);start_stream(&s,"queue",900000,7);assert(g_stream.id==1&&g_stream.gen==7&&g_stream.active&&g_stream.fd>=0);assert(job_prefix(&g_stream)==4096&&g_stream.done_bytes==4096);assert(g_stream.keep_queued&&g_queue_n==0);assert(sceIoLseek(g_stream.fd,0,PSP_SEEK_END)==4096);release_stream();assert(g_queue_n==1&&g_queue[0].id==1&&g_queue[0].state==DL_PENDING);assert(!strcmp(g_urls[0],"queue"));}
else if(!strcmp(argv[1],"persist")){enqueue(1);enqueue(2);g_queue[0].state=DL_ACTIVE;g_queue[0].pct=43;assert(dl_queue_pause(1)==0);queue_save();assert(!g_queue_dirty);g_queue_n=0;queue_load();assert(g_queue_n==2);assert(g_queue[0].id==1&&g_queue[0].state==DL_PAUSED&&g_queue[0].pct==43);assert(g_queue[1].id==2&&g_queue[1].state==DL_PAUSED);assert(!strcmp(g_urls[1],"queue"));assert(!opens);assert(dl_queue_resume(0)==0);assert(g_queue[0].state==DL_PENDING);}
else if(!strcmp(argv[1],"corrupt")||!strcmp(argv[1],"oversize")||!strcmp(argv[1],"truncated")){enqueue(1);queue_save();FILE*f=fopen(APP_DIR "download-queue.bin","r+b");assert(f);if(!strcmp(argv[1],"corrupt")){fseek(f,25,SEEK_SET);fputc(0xff,f);}else if(!strcmp(argv[1],"oversize")){fseek(f,0,SEEK_END);for(int i=0;i<60000;i++)fputc(0,f);}else{fclose(f);f=fopen(APP_DIR "download-queue.bin","wb");fputs("MDLQ",f);}fclose(f);g_queue_n=0;queue_load();assert(!g_queue_n);}
else if(!strcmp(argv[1],"backup")){enqueue(1);queue_save();enqueue(2);queue_save();FILE*f=fopen(APP_DIR "download-queue.bin","wb");assert(f);fputs("broken",f);fclose(f);g_queue_n=0;queue_load();assert(g_queue_n==1&&g_queue[0].id==1&&g_queue[0].state==DL_PAUSED);}
else if(!strcmp(argv[1],"capacity")){for(int i=0;i<32;i++)enqueue(i+1);queue_save();g_queue_n=0;queue_load();assert(g_queue_n==32);assert(g_queue[31].id==32);}
else if(!strcmp(argv[1],"workerpersist")){enqueue(1);assert(dl_queue_pause(0)==0);dl_thread(0,NULL);g_queue_n=0;queue_load();assert(g_queue_n==1&&g_queue[0].state==DL_PAUSED);}
else if(!strcmp(argv[1],"cachedrestore")){enqueue(1);enqueue(2);queue_save();cached_id=1;g_queue_n=0;queue_load();assert(g_queue_n==1&&g_queue[0].id==2&&g_queue[0].state==DL_PAUSED);assert(g_queue_dirty);assert(!opens);}
else if(!strcmp(argv[1],"cancelshift")){enqueue(1);enqueue(2);assert(dl_queue_pause(0)==0);dl_queue_cancel(0);queue_start_next();assert(g_queue[0].id==2);dl_thread(0,NULL);assert(g_queue_n==1&&g_queue[0].id==2);assert(g_queue[0].state!=DL_PAUSED);}
else if(!strcmp(argv[1],"clear")){enqueue(1);assert(dl_queue_pause(0)==0);queue_save();dl_queue_clear();dl_thread(0,NULL);g_queue_n=0;queue_load();assert(!g_queue_n);}
else assert(0);
puts("PASS production download pause/resume/persistence");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='dl-pause-') as tmp:
    tmp = Path(tmp)
    c = tmp / 'test.c'
    exe = tmp / 'test.exe'
    c.write_text(preamble + source + tail, encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c11', '-O0', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-variable', '-I', str(ROOT / 'src'), str(c), '-o', str(exe)], check=True)
    failures = []
    for case in ['pending', 'active', 'skip', 'allpaused', 'target', 'pausedplay', 'persist', 'corrupt', 'oversize', 'truncated', 'backup', 'capacity', 'workerpersist', 'clear', 'cachedrestore', 'cancelshift']:
        case_dir = tmp / case
        (case_dir / 'fixture/cache').mkdir(parents=True)
        result = subprocess.run([str(exe), case], cwd=case_dir, capture_output=True, text=True, timeout=10)
        print(case, 'PASS' if not result.returncode else 'FAIL', result.stderr)
        if result.returncode:
            failures.append(case)
    assert not failures, failures
    print('PASS 16 production queue cases')
