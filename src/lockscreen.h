#pragma once
#include <stddef.h>

// Keeps COSMIC's lock and login screen matching the animated wallpaper: COSMIC shows the
// background from its own config there, so driftpaper exports one still per monitor and points
// that config at them.
int lockscreen_supported(void);
void lockscreen_prepare(void);
void lockscreen_image_path(const char *output, char *out, size_t size);
int lockscreen_apply(const char *const *outputs, int count);
