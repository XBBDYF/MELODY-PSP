#ifndef MELODY_POWER_POLICY_H
#define MELODY_POWER_POLICY_H
/* Pure policy: unsigned millisecond arithmetic; no PSP or I/O dependencies. */
typedef struct {
    int eco, timeout_seconds, clock_mhz, blanked, waking;
    unsigned last_input_ms, last_render_ms;
} MelodyPowerPolicy;
static inline void melody_power_policy_init(MelodyPowerPolicy *p, unsigned now) {
    p->eco=0; p->timeout_seconds=0; p->clock_mhz=0;
    p->blanked=0; p->waking=0;
    p->last_input_ms=now; p->last_render_ms=now;
}
/* Return 1 when the wake gesture must not be passed to the application. */
static inline int melody_power_policy_tick(MelodyPowerPolicy *p, unsigned now,
                                           int input, int movie, int keep_awake) {
    if (movie || keep_awake) {
        p->last_input_ms=now;
        p->blanked=0;
    }
    if (input) {
        p->last_input_ms=now;
        if (p->blanked) { p->blanked=0; p->waking=1; }
        return p->waking;
    }
    p->waking=0;
    if (!movie && !keep_awake && p->timeout_seconds>0 &&
        (unsigned)(now-p->last_input_ms)>=(unsigned)p->timeout_seconds*1000u)
        p->blanked=1;
    return 0;
}
static inline int melody_power_clock_target(const MelodyPowerPolicy *p,
                                             int movie, int busy, int initial) {
    int target=p->clock_mhz?p->clock_mhz:(p->eco?222:initial);
    if (movie) return 333;
    if (busy && target<266) return 266;
    return target;
}
static inline int melody_power_render_due(MelodyPowerPolicy *p, unsigned now, int movie) {
    unsigned period;
    if (movie) return 1;
    if (p->blanked) return 0;
    if (!p->eco) return 1; /* Preserve the existing VBlank pacing. */
    period=(unsigned)(now-p->last_input_ms)<2000u?16u:33u;
    if ((unsigned)(now-p->last_render_ms)<period) return 0;
    p->last_render_ms=now;
    return 1;
}
#endif
