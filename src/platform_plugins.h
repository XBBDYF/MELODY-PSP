#ifndef MELODY_PLATFORM_PLUGINS_H
#define MELODY_PLATFORM_PLUGINS_H
/* Read-only user-mode probing. Hidden kernel modules remain unknown, not guessed. */
void platform_plugins_probe(void);
const char *platform_plugins_summary(void);
const char *platform_plugins_wifi_status(void);
#endif
