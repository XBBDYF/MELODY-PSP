#ifndef MELODY_POWER_RUNTIME_H
#define MELODY_POWER_RUNTIME_H
void power_runtime_init(void);
void power_runtime_shutdown(void);
int power_runtime_tick(unsigned now_ms,int input,int movie,int busy,int keep_awake);
int power_runtime_render_due(unsigned now_ms,int movie);
int power_runtime_eco(void);
int power_runtime_timeout(void);
int power_runtime_clock(void);
int power_runtime_effective_clock(void);
int power_runtime_blanked(void);
/* Strict validation, persist atomically; return <0 on invalid/save failure. */
int power_runtime_set(const char *key,int value);
int power_runtime_theme_value(const char *id);
int power_runtime_backlight_supported(void);
int power_runtime_theme_option(const char *id,int value);
int power_runtime_cycle(int setting_id,int direction);
#endif
