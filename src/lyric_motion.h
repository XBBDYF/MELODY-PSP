#ifndef MELODY_LYRIC_MOTION_H
#define MELODY_LYRIC_MOTION_H
/* Pure, allocation-free. dt and seconds are seconds; positions are line units. */
float lyric_motion_step(float value, float target, float dt, float seconds);
float lyric_scroll_position(float current, float target, float dt, int line_gap_ms);
#endif
