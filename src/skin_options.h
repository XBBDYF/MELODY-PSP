#ifndef MELODY_SKIN_OPTIONS_H
#define MELODY_SKIN_OPTIONS_H

/* Loaded only when applying the active skin. All values are integers. */
enum { SKIN_OPTION_TOGGLE = 0, SKIN_OPTION_CHOICE = 1, SKIN_OPTION_RANGE = 2 };
#define SKIN_OPTIONS_MAX 12

typedef struct SkinOption {
    char id[33], label[64];
    int type;
    int value, default_value, min, max, step, option_count;
    char options[8][64];
} SkinOption;

void skin_options_load(const char *root, const char *id);
void skin_options_reset(void);
int skin_options_count(void);
const SkinOption *skin_options_at(int index);
int skin_options_value(const char *id, int fallback);
/* 0: saved (or unchanged); -1: invalid input; -2: save failed, old value retained. */
int skin_options_adjust(int index, int direction);
int skin_options_set(int index, int value);

#endif
