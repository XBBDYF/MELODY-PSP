#include "lyric_motion.h"
#include <math.h>

float lyric_motion_step(float value, float target, float dt, float seconds)
{
    if (!isfinite(target)) return isfinite(value) ? value : 0.0f;
    if (!isfinite(value)) return target;
    if (!(dt > 0.0f) || !isfinite(dt)) return value;
    if (!(seconds > 0.0f) || !isfinite(seconds)) return target;
    /* Exponential approach is frame-rate independent and never overshoots. */
    float amount = 1.0f - expf(-dt / seconds);
    float next = value + (target - value) * amount;
    return fabsf(target - next) < 0.001f ? target : next;
}

float lyric_scroll_position(float current, float target, float dt, int line_gap_ms)
{
    float seconds = 0.16f;
    if (line_gap_ms > 0 && line_gap_ms < 440)
        seconds = line_gap_ms * 0.0005f;
    if (seconds < 0.04f) seconds = 0.04f;
    /* Seek jumps beyond a viewport snap; ordinary forward/backward changes ease. */
    if (fabsf(target - current) > 12.0f) return target;
    return lyric_motion_step(current, target, dt, seconds);
}
