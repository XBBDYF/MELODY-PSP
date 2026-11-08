"""Real C gallery/JSON with offline network fixtures; no PSP or live HTTP."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "update_gallery.h"
static const char *manifest;
static int expected, requests, badpng, truncation, cancel_fetch;
int http_get_expect(void){return expected;}
void net_skip_cookie(int n){(void)n;}
int http_get(const char*u,char*out,int cap,int*n,int t,char*e,int ec){
 unsigned char png[33]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,1,224,0,0,0,240,8,6};
 (void)t;(void)e;(void)ec;requests++;
 assert(strstr(u,"http://melody-1320953126.cos.ap-nanjing.myqcloud.com/melody/update-preview/3.5.0/")==u);
 if(strstr(u,"index.json")){*n=(int)strlen(manifest);memcpy(out,manifest,*n);}
 else {*n=33;memcpy(out,png,33);if(badpng)out[16]=1;}
 assert(*n<cap);out[*n]=0;expected=*n+truncation;if(cancel_fetch)update_gallery_shutdown();return 0;
}
int main(int argc,char**argv){(void)argc;
 manifest="{\"schema\":1,\"version\":\"3.5.0\",\"revision\":\"r2\",\"slides\":[{\"file\":\"01.png\"},{\"file\":\"02.png\"}]}";
 update_gallery_load();assert(!update_gallery_active());
 update_gallery_request_for_version("../bad","r2");assert(!update_gallery_pending());
 update_gallery_request_for_version("3.5.0","r2");assert(update_gallery_pending());assert(requests==0);
 if(!strcmp(argv[1],"invalid"))manifest="{\"schema\":1,\"version\":\"3.5.0\",\"revision\":\"r2\",\"slides\":[{\"file\":\"../01.png\"}]}";
 if(!strcmp(argv[1],"revision"))manifest="{\"schema\":1,\"version\":\"3.5.0\",\"revision\":\"r1\",\"slides\":[{\"file\":\"01.png\"}]}";
 if(!strcmp(argv[1],"many"))manifest="{\"schema\":1,\"version\":\"3.5.0\",\"revision\":\"r2\",\"slides\":[{\"file\":\"1.png\"},{\"file\":\"2.png\"},{\"file\":\"3.png\"},{\"file\":\"4.png\"},{\"file\":\"5.png\"},{\"file\":\"6.png\"},{\"file\":\"7.png\"},{\"file\":\"8.png\"},{\"file\":\"9.png\"}]}";
 if(!strcmp(argv[1],"empty"))manifest="{\"schema\":1,\"version\":\"3.5.0\",\"revision\":\"r2\",\"slides\":[]}";
 if(!strcmp(argv[1],"oversize"))badpng=1;
 if(!strcmp(argv[1],"truncated"))truncation=1;
 if(!strcmp(argv[1],"cancel"))cancel_fetch=1;
 update_gallery_fetch();
 if(strcmp(argv[1],"normal")){assert(!update_gallery_active());assert(!update_gallery_pending());return 0;}
 assert(update_gallery_active());assert(!update_gallery_pending());
 assert(update_gallery_input(0x2000));assert(!update_gallery_active());
 update_gallery_request_for_version("3.5.0","r2");assert(update_gallery_pending());update_gallery_fetch();assert(update_gallery_active());
 assert(update_gallery_input(0x20));assert(update_gallery_active());assert(update_gallery_input(0x20));assert(!update_gallery_active());
 update_gallery_request_for_version("3.5.0","r2");assert(!update_gallery_pending());
 update_gallery_request_for_version("3.5.0","r3");assert(update_gallery_pending());update_gallery_shutdown();assert(!update_gallery_pending());
 update_gallery_load();assert(update_gallery_pending());update_gallery_shutdown();
 update_gallery_request_for_version("123456789012345678901234567890123456789012345678","123456789012345678901234567890123456789012345678");assert(update_gallery_pending());update_gallery_shutdown();update_gallery_load();assert(update_gallery_pending());update_gallery_shutdown();return 0;
}
'''
class UpdateGalleryTests(unittest.TestCase):
    def test_real_module_contract(self):
        self.assertTrue((ROOT / 'src/update_gallery.c').exists(), 'gallery module missing')
        compiler = shutil.which('gcc') or shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler unavailable')
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            (tmp / 'harness.c').write_text(HARNESS, encoding='utf-8')
            exe = tmp / 'gallery.exe'
            subprocess.run([compiler, '-std=c99', '-Wall', '-Wextra', '-Werror', '-DUPDATE_GALLERY_HOST_TEST', '-DAPP_DIR="./"', '-I', str(ROOT / 'src'), str(tmp / 'harness.c'), str(ROOT / 'src/update_gallery.c'), str(ROOT / 'src/json.c'), '-o', str(exe)], check=True)
            for case in ('normal', 'invalid', 'revision', 'oversize', 'truncated', 'many', 'empty', 'cancel'):
                with self.subTest(case=case):
                    directory = tmp / case
                    directory.mkdir()
                    subprocess.run([str(exe), case], cwd=directory, check=True)
if __name__ == '__main__':
    unittest.main()

# Production integration: only successful bundle installs create revision pending.
_loader=(ROOT/'tools/prxloader/loader.c').read_text(encoding='utf-8')
assert 'if(bundle>0)'in _loader and 'update-preview-pending.txt'in _loader and 'MB_MARK'in _loader
_api=(ROOT/'src/api.c').read_text(encoding='utf-8');assert 'job==JOB_NONE&&update_gallery_pending()'in _api
_main=(ROOT/'src/main.c').read_text(encoding='utf-8');assert 'update_gallery_load();'in _main and 'update_gallery_input(pressed)'in _main and 'update_gallery_draw(g_app.time)'in _main
print('PASS success-only bundle marker, idle sole HTTP worker and main gallery input/render integration')
