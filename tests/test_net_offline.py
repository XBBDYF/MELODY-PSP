"""Exercise production offline gate; check public boundaries remain guarded."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1]
s=(R/'src/net.c').read_text(encoding='utf-8')
assert 'void net_set_offline(int offline)' in s, 'network final offline gate missing'
a=s.index('/* Offline/control gate:');b=s.index('/* 全局打断标志',a)
code=s[a:b]
pre='''#include <assert.h>\n#include <stddef.h>\nstatic int disconnects,drops,shutdowns;\nstatic unsigned sceKernelCpuSuspendIntr(void){return 0;}\nstatic void sceKernelCpuResumeIntr(unsigned x){(void)x;}\nstatic int sceNetApctlDisconnect(void){disconnects++;return 0;}\nstatic void ka_close_all(void){drops++;}\nstatic void shutdown_active(void){shutdowns++;}\nstatic int g_net_ok=1;\nstatic void dialog_retire_network(int connected){(void)connected;}\n'''
main='''int main(void){assert(!net_offline());assert(!network_blocked());assert(control_enter());assert(net_dialog_begin()==0);assert(network_blocked());assert(!control_enter());control_leave();assert(net_dialog_begin()==1);net_dialog_end(1);assert(control_enter());control_leave();net_set_offline(1);assert(net_offline());assert(disconnects==1&&drops==2&&shutdowns==2);assert(!control_enter());assert(net_dialog_begin()<0);net_set_offline(0);assert(!net_offline());assert(control_enter());control_leave();return 0;}'''
with tempfile.TemporaryDirectory() as t:
 t=Path(t);c=t/'t.c';e=t/'t.exe';c.write_text(pre+code+main,encoding='utf-8');subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(e)],check=True);subprocess.run([str(e)],check=True)
print('PASS offline/control ownership final gate')
