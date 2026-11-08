"""Use production byte accounting/rate sampler, including concurrent-counter wrap."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/net-speed-test';O.mkdir(exist_ok=True)
s=(R/'src/net.c').read_text(encoding='utf-8');a=s.index('static volatile unsigned net_rx_bytes;');b=s.index('static int recv_wait(',a)
code='''#include <assert.h>
#include <stdio.h>
static unsigned now=100;
static unsigned sceKernelCpuSuspendIntr(void){return 0;}
static void sceKernelCpuResumeIntr(unsigned n){(void)n;}
static unsigned sceKernelGetSystemTimeLow(void){return now;}
'''+s[a:b]+'''
int main(void){net_speed_tick();net_received(65536);now+=500000;net_speed_tick();assert(net_speed_kbps()==128);net_received(-1);now+=500000;net_speed_tick();assert(net_speed_kbps()==0);net_rx_bytes=0xfffffff0u;net_rate_bytes=net_rx_bytes;net_received(1040);now+=1000000;net_speed_tick();assert(net_speed_kbps()==1);puts("PASS production shared network counter:128KB/s, idle0, error excludes bytes, 32bit counter wrap");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
