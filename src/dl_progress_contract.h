#ifndef MELODY_DL_PROGRESS_CONTRACT_H
#define MELODY_DL_PROGRESS_CONTRACT_H

/* Download progress is a user-visible number, so it obeys three rules:
 *   1. never exceeds the declared content length (a retried/refetched hole must
 *      not be counted twice),
 *   2. never reports 100% while the job is still writing,
 *   3. the byte figure shown next to the percent is derived from the same number.
 * These used to disagree: a 4,148,706 byte job logged "done=4,155,726" and
 * finished as "4063KB" for a 4051KB file, so the row could read >100%. */

static inline long long dl_count_new_bytes(long long from, long long to, long long total)
{
    if (from < 0) from = 0;
    if (to <= from) return 0;
    if (total > 0 && to > total) to = total;
    return to > from ? to - from : 0;
}

static inline long long dl_clamp_done(long long done, long long total)
{
    if (done < 0) return 0;
    if (total > 0 && done > total) return total;
    return done;
}

static inline int dl_pct_from_bytes(long long done, long long total, int complete)
{
    if (complete) return 100;
    if (total <= 0) return 0;
    done = dl_clamp_done(done, total);
    {
        int pct = (int)(done * 100 / total);
        if (pct < 0) pct = 0;
        if (pct > 99) pct = 99;
        return pct;
    }
}

/* Bytes implied by a percentage; keeps the "1.8/4.0MB" label consistent with it. */
static inline long long dl_bytes_from_pct(int pct, long long total)
{
    if (total <= 0 || pct <= 0) return 0;
    if (pct >= 100) return total;
    return total * pct / 100;
}

#endif
