/* Local consent and plain-text announcements. No UI or thread creation. */
#ifndef MELODY_COMMUNITY_H
#define MELODY_COMMUNITY_H

#define COMMUNITY_ANNOUNCEMENT_ID_MAX 64
/* UTF-8 byte budget; also bounds the character count to at most 2048. */
#define COMMUNITY_ANNOUNCEMENT_TEXT_MAX 2048
#define COMMUNITY_ANNOUNCEMENT_DEFAULT_IGNORE 1

typedef struct {
    char id[COMMUNITY_ANNOUNCEMENT_ID_MAX + 1];
    char title[257];
    char text[COMMUNITY_ANNOUNCEMENT_TEXT_MAX + 1];
} CommunityAnnouncement;

enum {
    COMMUNITY_ANNOUNCEMENT_IDLE = 0,
    COMMUNITY_ANNOUNCEMENT_CHECKING,
    COMMUNITY_ANNOUNCEMENT_NONE,
    COMMUNITY_ANNOUNCEMENT_READY,
    COMMUNITY_ANNOUNCEMENT_ERROR
};

/* Startup/main, BEFORE starting the API worker. Local files only.
 * accept returns 0 only after the consent marker was successfully persisted.
 * Refusal is handled by caller exiting; no network may run before consent. */
int community_privacy_accepted(void);
int community_privacy_accept(void);
const char *community_privacy_text(void);

/* SINGLE API WORKER ONLY: check once per application session. No-op without
 * consent. Reads announcement-url.txt first, else derives announcement.txt
 * from the directory of the manifest URL in local update.txt.
 * Main queues dismiss to this SAME worker; default checkbox value is 1.
 * dismiss(1) persists ID; dismiss(0) hides only for this session.
 * Returns 0 on success, -1 on fetch/parse/storage error. */
int community_announcement_check(void);
int community_announcement_manual_check(void);
int community_announcement_dismiss(int dont_show_again);

/* MAIN READ ONLY. Published payload is immutable until an explicit worker manual check
 * while the announcement UI is closed; get returns NULL unless state is READY. State is published
 * only after all payload writes. Never start a second check this session. */
int community_announcement_state(void);
const CommunityAnnouncement *community_announcement_get(void);

/* Pure parser, no IO. Format: ID LF title LF body. CRLF accepted.
 * Strict ASCII ID [A-Za-z0-9._-], max 64 bytes. Title + body <= 2048 bytes;
 * reject truncation, embedded NUL and non-text controls (except LF/TAB).
 * Returns 1 for an ignored ID, 0 for a new ID, -1 for malformed input.
 * Output is only modified for a valid new ID. */
int community_announcement_parse(const char *data, int length,
                                 const char *ignored_id,
                                 CommunityAnnouncement *out);
#endif
