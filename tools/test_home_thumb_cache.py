"""Host regression for production startup thumbnail code; pixels are synthetic.
Real URL/hash metadata comes from the preserved 20261106 home-cards.bin.
This test never reads or writes the PSP drive.
"""
import pathlib
import re
import struct
import subprocess
import tempfile
import json

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / 'src/api.c').read_text(encoding='utf-8-sig')
assert 'static void home_thumbs_restore(void)' in source, 'missing network-independent startup thumbnail restore'
block = source[source.index('#define THUMB_MAX 24'):source.index('int api_thumb_slot(')]
block += '\n' + re.search(r'const CardList \*api_home_cards\(void\) \{[^}]+\}', source).group(0) + '\n'
blob = (ROOT / 'backups/APP339_LOG_20261106_123400/home-cards.bin').read_bytes()
magic, size = struct.unpack_from('<II', blob)
assert magic == 0x48434c31 and size == 15992
stride = (size - 56) // 24
urls = [blob[8+i*stride+72:8+i*stride+592].split(b'\0')[0].decode() for i in range(12)]
expected = [2856143893,3892573607,2801709859,988832876,3989529887,1099555848,4651764,1840620039,4118842807,2244086009,4087141878,2609852983]
header = r'''
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <assert.h>
#include <stdint.h>
#define APP_DIR ""
typedef struct { char cover[520]; } Card;
typedef struct { Card items[24]; int count; } CardList;
static CardList g_home_cards, g_cards;
static int io_depth, reads;
static void io_lock(void) { assert(io_depth++ == 0); }
static void io_unlock(void) { assert(--io_depth == 0); }
static void sceIoMkdir(const char *s, int m) { (void)s;(void)m; }
static void safe_copy(char *d, size_t n, const char *s) { snprintf(d,n,"%s",s); }
static void mlog(const char *s,...) { (void)s; }
'''
rows = '\n'.join('safe_copy(g_home_cards.items[%d].cover,520,%s);' % (i,json.dumps(u)) for i,u in enumerate(urls))
tail = r'''
int main(void) {
    /* Production cache is now partition-owned; provide pixels for disk tests. */
    static Thumb fixture[THUMB_MAX];
    home_thumbs_restore(); /* Allocation failure / suspended cache is harmless. */
    g_thumbs=fixture;
    char key[620], secure[620];
    unsigned short pixels[THUMB_W*THUMB_W];
    unsigned hashes[12] = { EXPECTED };
    g_home_cards.count=12;
    ROWS
    g_cards=g_home_cards;
    memset(&g_cards,0,sizeof(g_cards));
    assert(api_home_cards()->count==12);
    assert(!strcmp(api_home_cards()->items[0].cover,g_home_cards.items[0].cover));
    for(int i=0;i<12;i++) {
        image_request_url(g_home_cards.items[i].cover,THUMB_W,key,sizeof(key));
        assert(hash_url(key)==hashes[i]);
        snprintf(secure,sizeof(secure),"https://%s?imageView&ignored=1",g_home_cards.items[i].cover+7);
        char canonical[620];image_request_url(secure,THUMB_W,canonical,sizeof(canonical));
        assert(!strcmp(key,canonical));
        for(int j=0;j<THUMB_W*THUMB_W;j++)pixels[j]=(unsigned short)(i*100+j);
        thumb_disk_save(key,pixels);
    }
    home_thumbs_restore();
    for(int i=0;i<12;i++) {
        assert(g_thumbs[i].state==2);
        assert(!strcmp(g_thumbs[i].url,g_home_cards.items[i].cover));
        assert(g_thumbs[i].px[17]==i*100+17);
    }
    /* Ready entries survive API reinitialization without reading disk again. */
    for(int i=0;i<12;i++) {
        image_request_url(g_home_cards.items[i].cover,THUMB_W,key,sizeof(key));
        char path[72];snprintf(path,sizeof(path),"cache/thumb/t%u.565",hash_url(key)%256);remove(path);
    }
    home_thumbs_restore();
    for(int i=0;i<12;i++)assert(g_thumbs[i].state==2);
    /* Missing, collision and truncation must not publish pixels. */
    memset(g_thumbs,0,sizeof(fixture));home_thumbs_restore();
    for(int i=0;i<12;i++)assert(g_thumbs[i].state!=2);
    image_request_url(g_home_cards.items[0].cover,THUMB_W,key,sizeof(key));
    char path[72];snprintf(path,sizeof(path),"cache/thumb/t%u.565",hash_url(key)%256);
    FILE *f=fopen(path,"wb");unsigned wrong=123;fwrite(&wrong,4,1,f);fclose(f);
    assert(!thumb_disk_load(key,pixels));
    f=fopen(path,"wb");wrong=hash_url(key);fwrite(&wrong,4,1,f);fclose(f);
    assert(!thumb_disk_load(key,pixels));
    puts("PASS: 12 preserved real URL/hash keys; synthetic RGB565 restoration before worker; ready reuse; missing/collision/truncation rejection");
}
'''.replace('EXPECTED',','.join(str(x)+'u' for x in expected)).replace('ROWS',rows)
with tempfile.TemporaryDirectory(prefix='melody-home-thumb-') as tmp:
    tmp=pathlib.Path(tmp)
    (tmp/'cache/thumb').mkdir(parents=True)
    (tmp/'test.c').write_text(header+block+tail,encoding='utf-8')
    subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable',str(tmp/'test.c'),'-o',str(tmp/'test.exe')],check=True)
    subprocess.run([str(tmp/'test.exe')],cwd=tmp,check=True)
# Initialization must restore local covers before starting the network worker.
init=source[source.index('void api_init(void)'):source.index('int api_shutdown(void)')]
assert init.index('home_thumbs_restore();') < init.index('sceKernelStartThread(')
assert 'g_home_cards=saved' in source
assert 'g_home_cards=g_cards' in source
print('PASS: startup ordering and independent home snapshot update points')
