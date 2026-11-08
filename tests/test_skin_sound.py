"""Exercise real WAV parser/cache/mixer via host shared C build."""
import ctypes
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]

class SkinSoundTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        dll = cls.root / 'sound.dll'
        shim = cls.root / 'sound_shim.c'
        shim.write_text(r'''
#include <stdlib.h>
static int allocations;
static void *tracked_malloc(size_t n) { void *p=malloc(n); if(p) ++allocations; return p; }
static void tracked_free(void *p) { if(p) --allocations; free(p); }
#define malloc tracked_malloc
#define free tracked_free
#include "skin_sound.c"
int sound_test_allocations(void) { return allocations; }
''', encoding='ascii')
        result = subprocess.run(['gcc', '-shared', '-O2', '-Wall', '-Wextra', '-Werror',
                                 '-I', str(ROOT / 'src'), str(shim),
                                 str(ROOT / 'src/json.c'), '-o', str(dll)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.lib = ctypes.CDLL(str(dll))
        cls.lib.skin_sound_set_root.argtypes = [ctypes.c_char_p]
        cls.lib.skin_sound_play.argtypes = [ctypes.c_char_p]
        cls.lib.skin_sound_mix.argtypes = [ctypes.POINTER(ctypes.c_short), ctypes.c_int, ctypes.c_int, ctypes.c_int]

    @classmethod
    def tearDownClass(cls):
        cls.lib.skin_sound_release()
        cls.lib = None
        # Windows keeps loaded DLL handles until process exit.
        try:
            cls.tmp.cleanup()
        except PermissionError:
            pass

    def setUp(self):
        self.lib.skin_sound_release()
        (self.root / 'assets').mkdir(exist_ok=True)

    def load(self, declarations, samples=(32000, -32000, 1000), rate=44100, channels=1):
        with wave.open(str(self.root / 'assets/move.wav'), 'wb') as f:
            f.setnchannels(channels)
            f.setsampwidth(2)
            f.setframerate(rate)
            f.writeframes(struct.pack('<' + 'h' * len(samples), *samples))
        (self.root / 'skin.json').write_text(json.dumps({'assets': declarations}), encoding='ascii')
        self.lib.skin_sound_set_root(str(self.root).encode())

    def standard(self, **kwargs):
        self.load([{'id': 'move', 'type': 'sound', 'file': 'assets/move.wav'}], **kwargs)

    def mix(self, values, frames, channels=1, rate=44100):
        data = (ctypes.c_short * len(values))(*values)
        self.lib.skin_sound_mix(data, frames, channels, rate)
        return list(data)

    def test_mono_stereo_gain_saturation_and_completion(self):
        self.standard()
        self.assertEqual(self.lib.skin_sound_play(b'move'), 1)
        self.assertEqual(self.mix([32000, -32000, 0], 3), [32767, -32768, 250])
        self.assertEqual(self.lib.skin_sound_active(), 0)
        self.lib.skin_sound_play(b'move')
        self.assertEqual(self.mix([0] * 6, 3, 2), [8000, 8000, -8000, -8000, 250, 250])

    def test_stop_release_retrigger_and_invalid_output(self):
        self.standard()
        self.lib.skin_sound_play(b'move')
        self.assertEqual(self.mix([123], 1, rate=22050), [123])
        self.assertEqual(self.mix([0], 1), [8000])
        self.lib.skin_sound_play(b'move')
        self.assertEqual(self.mix([0], 1), [8000])
        self.lib.skin_sound_stop()
        self.assertEqual(self.mix([7], 1), [7])
        self.lib.skin_sound_release()
        self.assertEqual(self.lib.skin_sound_play(b'move'), 0)
        self.assertEqual(self.lib.skin_sound_active(), 0)

    def test_repeated_root_replacement_and_release_free_all_pcm(self):
        for _ in range(20):
            self.standard()
            self.assertEqual(self.lib.sound_test_allocations(), 1)
            self.lib.skin_sound_play(b'move')
            self.lib.skin_sound_release()
            self.assertEqual(self.lib.sound_test_allocations(), 0)
        self.standard()
        self.standard()
        self.assertEqual(self.lib.sound_test_allocations(), 1)
        self.lib.skin_sound_set_root(None)
        self.assertEqual(self.lib.sound_test_allocations(), 0)

    def test_wrong_formats_and_limits(self):
        for kw in ({'rate': 22050}, {'channels': 2}, {'samples': [1] * 15436}):
            self.standard(**kw)
            self.assertEqual(self.lib.skin_sound_play(b'move'), 0, kw.keys())
        self.standard()
        path = self.root / 'assets/move.wav'
        path.write_bytes(path.read_bytes()[:-1])
        self.lib.skin_sound_set_root(str(self.root).encode())
        self.assertEqual(self.lib.skin_sound_play(b'move'), 0)

    def test_existing_output_owner_reuses_channel_and_excludes_mv(self):
        source = (ROOT / 'src/audio.c').read_text(encoding='utf-8')
        start = source.index('static int audio_skin_silence(void)')
        end = source.index('static int audio_thread(', start)
        harness = r'''
#include <assert.h>
#include <string.h>
#define OUT_SAMPLES 1024
#define OUT_RATE 44100
#define PSP_AUDIO_NEXT_CHANNEL -1
#define PSP_AUDIO_FORMAT_STEREO 0
#define PSP_AUDIO_VOLUME_MAX 32768
static int g_quit, g_audio_faulted, g_mv_held, g_mv_request;
static int g_channel=-1, active=1, reserves, outputs, stops;
static short g_outbuf[2048];
static int skin_sound_active(void) { return active; }
static void skin_sound_stop(void) { active=0; ++stops; }
static int sceAudioChReserve(int c,int n,int f) {
    assert(c==-1 && n==1024 && f==0); ++reserves; return 3;
}
static void skin_sound_mix(short *p,int n,int c,int r) {
    assert(n==1024 && c==2 && r==44100);
    for(int i=0;i<2048;++i) assert(p[i]==0);
    p[0]=1234;
}
static void audio_skin_output(void);
static int sceAudioOutputBlocking(int c,int v,short *p) {
    assert(c==3 && v==32768 && p[0]==1234); ++outputs; return 0;
}
static void audio_skin_output(void) { sceAudioOutputBlocking(g_channel, PSP_AUDIO_VOLUME_MAX, g_outbuf); }
'''
        harness += source[start:end]
        harness += r'''
int main(void) {
    assert(audio_skin_silence()==1 && reserves==1 && outputs==1);
    assert(audio_skin_silence()==1 && reserves==1 && outputs==2);
    g_channel=-1; g_mv_held=1;
    assert(audio_skin_silence()==0 && reserves==1 && outputs==2);
    g_mv_held=0; g_mv_request=1;
    assert(audio_skin_silence()==0 && reserves==1);
    g_mv_request=0; g_audio_faulted=1;
    assert(audio_skin_silence()==0 && reserves==1);
    g_audio_faulted=0; g_quit=1;
    assert(audio_skin_silence()==0 && reserves==1);
    g_quit=0; active=0;
    assert(audio_skin_silence()==0 && reserves==1 && stops==0);
    return 0;
}
'''
        cfile = self.root / 'output_owner.c'
        exe = self.root / 'output_owner.exe'
        cfile.write_text(harness, encoding='utf-8')
        subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


    def test_real_metro_banks_through_actual_output_owner(self):
        source = (ROOT / 'src/audio.c').read_text(encoding='utf-8')
        start = source.index('static void audio_skin_output(void)')
        end = source.index('static int audio_thread(', start)
        harness = r"""
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "skin_sound.h"
#define OUT_SAMPLES 1024
#define OUT_RATE 44100
#define PSP_AUDIO_NEXT_CHANNEL -1
#define PSP_AUDIO_FORMAT_STEREO 0
#define PSP_AUDIO_VOLUME_MAX 32768
static int g_quit, g_audio_faulted, g_mv_held, g_mv_request;
static int g_channel=-1, reserves, outputs, output_peak;
static unsigned output_nonzero;
static short g_outbuf[2048];
static FILE *g_dump_out, *g_dump_dec;
static int g_dump_blocks;
static void mlog(const char *fmt, ...) { (void)fmt; }
static int sceAudioChReserve(int c,int n,int f) {
    assert(c==-1 && n==1024 && f==0); ++reserves; return 3;
}
static int sceAudioOutputBlocking(int c,int v,short *p) {
    assert(c==3 && v==32768); ++outputs;
    for(int i=0;i<1024;++i) {
        int a=abs(p[i*2]); assert(p[i*2]==p[i*2+1]);
        if(a) ++output_nonzero;
        if(a>output_peak) output_peak=a;
    }
    return 0;
}
"""
        harness += source[start:end]
        harness += r"""
int main(int argc,char **argv) {
    const char *names[]={"move","confirm","back"};
    assert(argc==2); skin_sound_set_root(argv[1]);
    assert(skin_sound_loaded_count()==3);
    g_dump_out=tmpfile(); assert(g_dump_out); g_dump_blocks=30;
    for(int i=0;i<3;++i) {
        unsigned before=skin_sound_mixed_frames();
        assert(skin_sound_play(names[i])==1);
        int guard=30;
        while(skin_sound_active() && --guard) assert(audio_skin_silence()==1);
        assert(guard && skin_sound_mixed_frames()>before);
    }
    assert(reserves==1 && outputs>3 && output_peak>3000);
    assert(output_nonzero==skin_sound_mixed_frames());
    assert(output_peak==skin_sound_peak() && skin_sound_play_count()==3);
    assert(ftell(g_dump_out)==(long)outputs*4096);
    rewind(g_dump_out);
    unsigned dumped_nonzero=0; short sample;
    while(fread(&sample,2,1,g_dump_out)==1) if(sample) ++dumped_nonzero;
    assert(dumped_nonzero==output_nonzero*2);
    fclose(g_dump_out); g_dump_out=NULL;
    printf("real bank loaded=3 plays=3 nonzero_frames=%u peak=%d outputs=%d\n",
           output_nonzero,output_peak,outputs);
    skin_sound_release(); return 0;
}
"""
        cfile = self.root / 'real_output.c'
        exe = self.root / 'real_output.exe'
        cfile.write_text(harness, encoding='utf-8')
        subprocess.run(['gcc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'src'), str(cfile), str(ROOT / 'src/skin_sound.c'),
                        str(ROOT / 'src/json.c'), '-o', str(exe)], check=True)
        for bank in (ROOT / 'skin_release/melody/skins/metro',
                     ROOT / 'skin_release/melody/skins/metro'):
            with self.subTest(bank=bank):
                subprocess.run([str(exe), str(bank)], check=True)

    def test_declared_identifiers_are_bounded_safe_ascii(self):
        for name in ('a' * 33, 'a/b', 'a.b', '音效', ''):
            self.load([{'id': name, 'type': 'sound', 'file': 'assets/move.wav'}])
            self.assertEqual(self.lib.skin_sound_play(name.encode('utf-8')), 0)
        name = 'a' * 32
        self.load([{'id': name, 'type': 'sound', 'file': 'assets/move.wav'}])
        self.assertEqual(self.lib.skin_sound_play(name.encode()), 1)

    def test_eight_sound_limit_and_malformed_manifest(self):
        self.load([{'id': str(i), 'type': 'sound', 'file': 'assets/move.wav'} for i in range(9)])
        self.assertEqual(self.lib.skin_sound_play(b'7'), 1)
        self.assertEqual(self.lib.skin_sound_play(b'8'), 0)
        (self.root / 'skin.json').write_text('{"assets":[', encoding='ascii')
        self.lib.skin_sound_set_root(str(self.root).encode())
        self.assertEqual(self.lib.skin_sound_play(b'7'), 0)
        self.assertEqual(self.lib.skin_sound_active(), 0)

    def test_only_declared_safe_assets_and_cache_budget(self):
        for file in ('../assets/move.wav', 'assets/../move.wav', 'assets/move.WAV', 'assets/a/b.wav'):
            self.load([{'id': 'move', 'type': 'sound', 'file': file}])
            self.assertEqual(self.lib.skin_sound_play(b'move'), 0)
        self.load([{'id': 'move', 'type': 'image', 'file': 'assets/move.wav'}])
        self.assertEqual(self.lib.skin_sound_play(b'move'), 0)
        self.load([{'id': str(i), 'type': 'sound', 'file': 'assets/move.wav'} for i in range(9)], samples=[1] * 15435)
        self.assertEqual(self.lib.skin_sound_play(b'5'), 1)
        self.assertEqual(self.lib.skin_sound_play(b'6'), 0)  # 7*30870 exceeds 192KiB
        self.standard()
        self.assertEqual(self.lib.skin_sound_play(b'5'), 0)
        self.lib.skin_sound_set_root(None)
        self.assertEqual(self.lib.skin_sound_play(b'move'), 0)

if __name__ == '__main__':
    unittest.main()
