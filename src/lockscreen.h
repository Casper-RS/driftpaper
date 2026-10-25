#pragma once
#include <stddef.h>

// Keeps COSMIC's lock and login screen matching the animated wallpaper: COSMIC shows the
// background from its own config there, so driftpaper exports one still per monitor and points
// that config at them.
int lockscreen_supported(void);
void lockscreen_prepare(void);
void lockscreen_image_path(const char *output, char *out, size_t size);
// Returns 1 when COSMIC's config was changed (cosmic-bg then reloads), 0 when it already pointed
// at these pictures, -1 on failure.
int lockscreen_apply(const char *const *outputs, int count);
