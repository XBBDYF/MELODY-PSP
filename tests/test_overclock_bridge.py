"""Compile the pure external-ownership state machine; no PSP/plugin execution."""
from pathlib import Path
import subprocess
import tempfile

R = Path(__file__).resolve().parents[1]
CODE = r'''
#include <assert.h>
#include "overclock_bridge.h"
int main(void) {
    OverclockBridgeConsent c = {1,1,1,1,46,1,0};
    assert(overclock_bridge_state()==OVERCLOCK_BRIDGE_OFF);
    assert(!overclock_bridge_owns_clock());
    assert(overclock_bridge_profile_count()==3);
    assert(overclock_bridge_profile_index(0)==17);
    assert(overclock_bridge_profile_nominal_khz(2)==501555);
    assert(overclock_bridge_enable(0,0)==OVERCLOCK_BRIDGE_CONFIRM_REQUIRED);
    c.generation=9;
    assert(overclock_bridge_enable(&c,0)==OVERCLOCK_BRIDGE_BAD_MODEL);
    c.generation=5;c.explicit_user_action=0;
    assert(overclock_bridge_enable(&c,0)==OVERCLOCK_BRIDGE_CONFIRM_REQUIRED);
    c.explicit_user_action=1;c.same_device_profile_confirmed=0;
    assert(overclock_bridge_enable(&c,0)==OVERCLOCK_BRIDGE_CONFIRM_REQUIRED);
    c.same_device_profile_confirmed=1;c.conflicting_power_profile=1;
    assert(overclock_bridge_enable(&c,0)==OVERCLOCK_BRIDGE_CONFLICT);
    c.conflicting_power_profile=0;c.max_tested_index=18;
    assert(overclock_bridge_enable(&c,0)==OVERCLOCK_BRIDGE_BAD_PROFILE);
    c.max_tested_index=19;
    assert(overclock_bridge_enable(&c,0)==0);
    assert(overclock_bridge_nominal_khz()==402888);
    assert(overclock_bridge_owns_clock());
    assert(overclock_bridge_requested_index()==17);
    assert(overclock_bridge_request_release()==0);
    assert(overclock_bridge_state()==OVERCLOCK_BRIDGE_RELEASE_PENDING);
    assert(overclock_bridge_nominal_khz()==0);
    assert(overclock_bridge_owns_clock());
    assert(overclock_bridge_acknowledge_release(0)==OVERCLOCK_BRIDGE_CONFIRM_REQUIRED);
    assert(overclock_bridge_enable(&c,0)==OVERCLOCK_BRIDGE_BAD_STATE);
    assert(overclock_bridge_acknowledge_release(1)==0);
    assert(!overclock_bridge_owns_clock());
    c.max_tested_index=46;
    assert(overclock_bridge_enable(&c,2)==0);
    overclock_bridge_on_resume();
    assert(overclock_bridge_state()==OVERCLOCK_BRIDGE_RELEASE_PENDING);
    assert(overclock_bridge_owns_clock());
    assert(overclock_bridge_acknowledge_release(1)==0);
    assert(overclock_bridge_enable(&c,3)==OVERCLOCK_BRIDGE_BAD_PROFILE);
    c.max_tested_index=47;
    assert(overclock_bridge_enable(&c,0)==OVERCLOCK_BRIDGE_BAD_PROFILE);
    assert(!overclock_bridge_owns_clock());
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='melody-oc-bridge-') as td:
    p = Path(td)
    (p/'test.c').write_text(CODE, encoding='utf-8')
    subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe', '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-I'+str(R/'src'), str(p/'test.c'),
                    str(R/'src/overclock_bridge.c'), '-o', str(p/'test.exe')], check=True)
    subprocess.run([str(p/'test.exe')], check=True)
print('PASS external OC bridge: consent, models, profiles, ownership, manual release, resume')
