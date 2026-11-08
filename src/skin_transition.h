#ifndef MELODY_SKIN_TRANSITION_H
#define MELODY_SKIN_TRANSITION_H
/* Shared by all native and scripted Metro page navigation. One page snapshot.
 * Closing starts at the first draw, excluding synchronous navigation loading. */
void skin_transition_begin_wait(int page,float time);
void skin_transition_request(int old_page,int new_page,float time);
/* Same-page body changes: request is synchronous; wait holds until ready.
 * Bounds are pixel rows [top,bottom), clipped to the screen. One snapshot. */
void skin_transition_request_section(int page,int top,int bottom,float time);
void skin_transition_wait_section(int page,int top,int bottom,float time);
void skin_transition_ready_section(int page,float time);
void skin_transition_draw(int page,float time);
int skin_transition_active(void);
int skin_transition_waiting(void);
void skin_transition_cancel(void);
void skin_transition_reset(void);
#endif
