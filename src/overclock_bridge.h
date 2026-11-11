#ifndef MELODY_OVERCLOCK_BRIDGE_H
#define MELODY_OVERCLOCK_BRIDGE_H
/* External opicker ownership only. No plugin loading, PLL writes or frequency probes.
 * Call on the foreground thread. Generation numbers are PSP hardware generations,
 * NOT sceKernelGetModel() enum values; unknown identity must be passed as 0. */
enum {
    OVERCLOCK_BRIDGE_OFF = 0,
    OVERCLOCK_BRIDGE_EXTERNAL = 1,
    OVERCLOCK_BRIDGE_RELEASE_PENDING = 2
};
enum {
    OVERCLOCK_BRIDGE_OK = 0,
    OVERCLOCK_BRIDGE_BAD_MODEL = -1,
    OVERCLOCK_BRIDGE_BAD_PROFILE = -2,
    OVERCLOCK_BRIDGE_CONFIRM_REQUIRED = -3,
    OVERCLOCK_BRIDGE_CONFLICT = -4,
    OVERCLOCK_BRIDGE_BAD_STATE = -5
};
/* All attestations are explicit user confirmations, not hardware detection.
 * max_tested_index is the integer from an existing opicker.id generated for this
 * exact PSP and upstream default table (PLL_DEFAULT_DEN=0x12). No file is created.
 * The plugin reserves two entries below that maximum. */
typedef struct {
    int generation;                 /* Supported: 1g, 2g, 3g, 5g only. */
    int default_table_confirmed;
    int plugin_enabled_confirmed;
    int same_device_profile_confirmed;
    int max_tested_index;
    int explicit_user_action;
    int conflicting_power_profile;  /* eco/extreme mode must pass 1. */
} OverclockBridgeConsent;
/* Nominal upstream table suggestions; selecting one never applies it. */
int overclock_bridge_profile_count(void);
int overclock_bridge_profile_index(int choice);
int overclock_bridge_profile_nominal_khz(int choice);
int overclock_bridge_enable(const OverclockBridgeConsent *consent, int choice);
int overclock_bridge_state(void);
/* Remains true during release pending: do not resume SDK clock writes or NULL FB. */
int overclock_bridge_owns_clock(void);
int overclock_bridge_nominal_khz(void);
int overclock_bridge_requested_index(void);
/* No kernel deactivate API exists. First request manual cancellation in opicker;
 * only after the user confirms cancellation (or reboot with plugin disabled) may
 * acknowledge_release() return ownership to normal app power profiles. */
int overclock_bridge_request_release(void);
int overclock_bridge_acknowledge_release(int explicit_user_confirmation);
/* Resume invalidates any assumption of active OC but retains clock ownership. */
void overclock_bridge_on_resume(void);
const char *overclock_bridge_deactivate_contract(void);
#endif
