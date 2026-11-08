"""PSP fixed-priority scheduling contract from actual thread creation sites.
This models runnable priorities, not physical WLAN throughput.
"""
from pathlib import Path
import re
R=Path(__file__).resolve().parents[1];s=(R/'src/mv.c').read_text(encoding='utf-8');audio=(R/'tools/mvd/patched/decoder/src/audio_playback.h').read_text(encoding='utf-8');pacer=(R/'tools/mvd/patched/decoder/src/rgb_pacer.h').read_text(encoding='utf-8')
receiver=int(re.search(r'#define MV_STREAM_PRIO\s+(0x[0-9a-fA-F]+)',s)[1],16)
decoder=int(re.search(r'sceKernelCreateThread\("mv_play",\s*mv_thread,\s*(0x[0-9a-fA-F]+)',s)[1],16)
audio_out=int(re.search(r'sceKernelCreateThread\("pcm_output",\s*audio_output_thread,\s*(0x[0-9a-fA-F]+)',audio)[1],16)
presenter=int(re.search(r'sceKernelCreateThread\("pts_present",\s*rgb_worker,\s*(0x[0-9a-fA-F]+)',pacer)[1],16)
ready={'video_decode':decoder,'ready_network_receive':receiver}
chosen=min(ready,key=ready.get);print('continuously runnable decoder vs ready receiver:',ready,'selected:',chosen)
assert chosen=='ready_network_receive','CPU-bound decode can starve network until pause: receiver has lower priority'
assert audio_out<receiver and presenter<receiver,'Audio output and presentation must keep higher priority than bulk receiving'
body=s[s.index('static int src_thread('):s.index('static const char *src_path(',s.index('static int src_thread('))]
assert 'sceKernelDelayThread(500)'in body,'Successful bounded receive/write must yield for decode'
print('PASS receive can preempt busy decode, below audio/presentation, bounded receive yields; physical throughput unmeasured')
