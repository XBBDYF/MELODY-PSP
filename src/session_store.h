#ifndef MELODY_SESSION_STORE_H
#define MELODY_SESSION_STORE_H

#include <string.h>

/* Callers supply fixed, distinct, same-directory application paths (no aliases).
 * The caller validates raw live content with its shared parser and passes 1 only
 * for a known-valid live session. This helper does not parse account content.
 * Calls must be serialized: validation and replacement must see the same live.
 * A false flag preserves backup but permits removal of live: the caller gives
 * up relying on that unvalidated live. A wrong false flag with no backup can
 * lose the old live on commit failure; a wrong true flag can retire good backup.
 * Neither validation nor these IO results prove media
 * health or guarantee recovery from power loss/device corruption.
 */
static int io_write_complete(int fd, const void *data, int size)
{
    const unsigned char *p = (const unsigned char *)data;
    int done = 0;
    while (done < size) {
        int n = sceIoWrite(fd, p + done, (SceSize)(size - done));
        if (n <= 0 || n > size - done) return -1;
        done += n;
    }
    return 0;
}

/* Return 0 after candidate commit, -1 on any reported failure. */
static int session_replace(const char *live, const char *part, const char *backup,
                           const void *data, int size, int live_is_valid)
{
    SceIoStat st;
    int fd, ok, moved = 0;
    if (!live || !part || !backup || !*live || !*part || !*backup ||
        !strcmp(live, part) || !strcmp(live, backup) || !strcmp(part, backup) ||
        !data || size <= 0) return -1;
    fd = sceIoOpen(part, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    ok = io_write_complete(fd, data, size);
    if (sceIoClose(fd) < 0) ok = -1;
    if (ok == 0 && sceIoSync("ms0:", 0) < 0) ok = -1;
    if (ok < 0) return -1;
    if (sceIoGetstat(live, &st) >= 0) {
        if (live_is_valid) {
            /* Only known-valid live can replace an older recovery backup. */
            if (sceIoGetstat(backup, &st) >= 0 && sceIoRemove(backup) < 0) return -1;
            if (sceIoRename(live, backup) < 0) return -1;
            moved = 1;
        } else {
            /* Backup may be the sole valid session: never remove or rename it.
             * FAT rename does not overwrite, so remove only the bad live. */
            if (sceIoRemove(live) < 0) return -1;
        }
    }
    if (sceIoRename(part, live) < 0) {
        /* Even if restore fails, backup is the recoverable old session. */
        if (moved) sceIoRename(backup, live);
        return -1;
    }
    /* Keep the prior valid session in backup after a successful commit too. */
    return 0;
}
#endif
