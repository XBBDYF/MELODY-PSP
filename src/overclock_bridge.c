#include "overclock_bridge.h"

/* Source: mcidclan/psp-beyond-444mhz overclock-picker/octable.h,
 * default multipliers0x12. Metadata only; no register values are copied. */
static const int profile_indices[] = {17, 27, 41};
static const int nominal_khz[] = {402888, 444000, 501555};
static int state; /* Default OFF. Deliberately no persistence or auto-enable. */
static int selected = -1;

int overclock_bridge_profile_count(void) { return 3; }
int overclock_bridge_profile_index(int choice) {
    return choice >= 0 && choice < 3 ? profile_indices[choice] : -1;
}
int overclock_bridge_profile_nominal_khz(int choice) {
    return choice >= 0 && choice < 3 ? nominal_khz[choice] : 0;
}
int overclock_bridge_enable(const OverclockBridgeConsent *c, int choice) {
    int index = overclock_bridge_profile_index(choice);
    if (!c || !c->explicit_user_action) return OVERCLOCK_BRIDGE_CONFIRM_REQUIRED;
    if (state == OVERCLOCK_BRIDGE_RELEASE_PENDING) return OVERCLOCK_BRIDGE_BAD_STATE;
    if (c->generation != 1 && c->generation != 2 &&
        c->generation != 3 && c->generation != 5) return OVERCLOCK_BRIDGE_BAD_MODEL;
    if (c->conflicting_power_profile) return OVERCLOCK_BRIDGE_CONFLICT;
    if (!c->default_table_confirmed || !c->plugin_enabled_confirmed ||
        !c->same_device_profile_confirmed) return OVERCLOCK_BRIDGE_CONFIRM_REQUIRED;
    /* Default table has 47 entries, indices 0..46. Plugin SAFETY_MARGIN is 2. */
    if (index < 0 || c->max_tested_index < 2 || c->max_tested_index > 46 ||
        index > c->max_tested_index - 2) return OVERCLOCK_BRIDGE_BAD_PROFILE;
    selected = choice;
    state = OVERCLOCK_BRIDGE_EXTERNAL;
    return OVERCLOCK_BRIDGE_OK;
}
int overclock_bridge_state(void) { return state; }
int overclock_bridge_owns_clock(void) { return state != OVERCLOCK_BRIDGE_OFF; }
int overclock_bridge_nominal_khz(void) {
    return state == OVERCLOCK_BRIDGE_EXTERNAL ?
        overclock_bridge_profile_nominal_khz(selected) : 0;
}
int overclock_bridge_requested_index(void) {
    return state == OVERCLOCK_BRIDGE_EXTERNAL ?
        overclock_bridge_profile_index(selected) : -1;
}
int overclock_bridge_request_release(void) {
    if (state != OVERCLOCK_BRIDGE_OFF) state = OVERCLOCK_BRIDGE_RELEASE_PENDING;
    return OVERCLOCK_BRIDGE_OK;
}
int overclock_bridge_acknowledge_release(int explicit_user_confirmation) {
    if (state != OVERCLOCK_BRIDGE_RELEASE_PENDING) return OVERCLOCK_BRIDGE_BAD_STATE;
    if (!explicit_user_confirmation) return OVERCLOCK_BRIDGE_CONFIRM_REQUIRED;
    state = OVERCLOCK_BRIDGE_OFF;
    selected = -1;
    return OVERCLOCK_BRIDGE_OK;
}
void overclock_bridge_on_resume(void) {
    if (state != OVERCLOCK_BRIDGE_OFF) state = OVERCLOCK_BRIDGE_RELEASE_PENDING;
}
const char *overclock_bridge_deactivate_contract(void) {
    return "Use external opicker: hold L+R and VOL- until index 0, then confirm; "
           "or reboot with the plugin disabled. This app cannot deactivate the plugin.";
}
