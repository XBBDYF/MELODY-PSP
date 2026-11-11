"""Compile real power policy; verify timeout, wake, clocks and render deadlines."""
from pathlib import Path
import subprocess
R = Path(__file__).resolve().parents[1]
O = R / '.tmp/power-policy-test'
O.mkdir(parents=True, exist_ok=True)
code = r'''
#include <assert.h>
#include <stdio.h>
#include "power_policy.h"
int main(void) {
    MelodyPowerPolicy p = {0};
    melody_power_policy_init(&p, 1000);
    /* No default blanking; choose explicit 30 second timeout. */
    assert(!melody_power_policy_tick(&p, 40000, 0, 0, 0));
    assert(!p.blanked);
    p.timeout_seconds = 30;
    melody_power_policy_tick(&p, 50000, 1, 0, 0);
    assert(!p.blanked);
    melody_power_policy_tick(&p, 79999, 0, 0, 0);
    assert(!p.blanked);
    melody_power_policy_tick(&p, 80000, 0, 0, 0);
    assert(p.blanked);
    /* First input only wakes. Held key remains swallowed until release. */
    assert(melody_power_policy_tick(&p, 80001, 1, 0, 0));
    assert(!p.blanked);
    assert(melody_power_policy_tick(&p, 80002, 1, 0, 0));
    assert(!melody_power_policy_tick(&p, 80003, 0, 0, 0));
    assert(!melody_power_policy_tick(&p, 80004, 1, 0, 0));
    /* A movie blocks blanking and enforces 333 regardless of low clock. */
    p.clock_mhz = 222;
    melody_power_policy_tick(&p, 999999, 0, 1, 0);
    assert(!p.blanked);
    assert(melody_power_clock_target(&p, 1, 0, 266) == 333);
    assert(melody_power_clock_target(&p, 0, 0, 266) == 222);
    p.clock_mhz = 0;
    assert(melody_power_clock_target(&p, 0, 0, 266) == 266);
    p.eco = 1;
    assert(melody_power_clock_target(&p, 0, 0, 333) == 222);
    /* Active network/decode startup gets a performance floor. */
    assert(melody_power_clock_target(&p, 0, 1, 333) >= 266);
    /* Render gate: 60Hz interactively, 30Hz after 2s idle; no swap while blank. */
    p.last_input_ms = 1000; p.last_render_ms = 3000;
    assert(!melody_power_render_due(&p, 3010, 0));
    assert(melody_power_render_due(&p, 3034, 0));
    p.blanked = 1;
    assert(!melody_power_render_due(&p, 9000, 0));
    assert(melody_power_render_due(&p, 9000, 1));
    /* Unsigned millisecond wrap is harmless. */
    melody_power_policy_init(&p, 0xfffffff0u);
    p.timeout_seconds = 30;
    melody_power_policy_tick(&p, 0x20u, 0, 0, 0);
    assert(!p.blanked);
    puts("PASS power policy: inactivity, wake swallowing, MV guard, clocks, render gate, wrap");
}
'''
(O/'test.c').write_text(code, encoding='utf-8')
subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-Wall','-Wextra','-Werror','-I'+str(R/'src'),str(O/'test.c'),'-o',str(O/'test.exe')],check=True)
subprocess.run([str(O/'test.exe')],check=True)
