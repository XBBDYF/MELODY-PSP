#ifndef MELODY_IME_UPDATE_H
#define MELODY_IME_UPDATE_H
/* Main thread starts/polls; private worker downloads and validates pure data. */
int ime_update_start(void);
void ime_update_poll(void);
void ime_update_cancel(void);
int ime_update_busy(void);
int ime_update_percent(void);
const char *ime_update_message(void);
#endif
