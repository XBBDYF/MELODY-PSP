#ifndef MELODY_INPUT_BUTTONS_H
#define MELODY_INPUT_BUTTONS_H
/* All application handlers receive logical Circle=confirm/Cross=cancel.
 * Translate physical samples ONCE; system utility dialogs keep physical input. */
void input_buttons_init(void);
unsigned input_buttons_translate(unsigned physical);
int input_buttons_set_mode(int mode); /* -1 system, 0 circle, 1 cross; persist before apply */
int input_buttons_mode(void);
int input_buttons_cross_confirm(void);
const char *input_buttons_confirm_label(void);
const char *input_buttons_cancel_label(void);
/* Hints only, never song/query/lyric data. Rotating scratch buffers. */
const char *input_buttons_hint(const char *text);
#endif
