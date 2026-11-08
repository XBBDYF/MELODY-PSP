#ifndef MELODY_UPDATE_GALLERY_H
#define MELODY_UPDATE_GALLERY_H
/* UI thread: load/request/active/input/draw/shutdown. API worker: pending/fetch.
 * fetch MUST run only on the existing sole HTTP/API worker, after normal jobs.
 * Stop/join that worker before final shutdown. No module-owned threads.
 */
void update_gallery_load(void);
int update_gallery_active(void);
int update_gallery_input(unsigned pressed);
void update_gallery_draw(float time);
void update_gallery_request_for_version(const char *version, const char *revision);
void update_gallery_fetch(void);
int update_gallery_pending(void);
void update_gallery_shutdown(void);
#endif
