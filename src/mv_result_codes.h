#ifndef MELODY_MV_RESULT_CODES_H
#define MELODY_MV_RESULT_CODES_H
/* Private App/PRX contract. Unconfirmed worker stop always outranks demux,
 * decode and user-cancel errors: its code/heap may still be in use. */
#define MV_ERROR_UNSAFE_STOP (-0x4d5601)
static inline int mv_result_requires_restart(int result)
{
    return result == MV_ERROR_UNSAFE_STOP;
}
static inline int mv_worker_cleanup_code(int result, int worker_result)
{
    return worker_result < 0 ? MV_ERROR_UNSAFE_STOP : result;
}
static inline int mv_normalize_result(int result, int cancelled)
{
    return mv_result_requires_restart(result) ? result : cancelled ? 0 : result;
}
#endif
