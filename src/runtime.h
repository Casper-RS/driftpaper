#pragma once
#include <time.h>

// Sandboxes such as Flatpak may hide WAYLAND_DISPLAY while still exposing the compositor's
// socket; this finds the socket and sets WAYLAND_DISPLAY. Returns 0 when one is available.
int runtime_find_wayland_display(void);

// Takes the single-instance lock. Returns a file descriptor to keep open, or -1 if another
// wallpaper already holds it.
int runtime_acquire_lock(void);
int runtime_wallpaper_running(void);

// Asks the running wallpaper to exit so a new one can take over, then waits for its lock.
// Returns the lock like runtime_acquire_lock(), or -1 if the old one did not let go in time.
int runtime_take_over(void);
// True when another wallpaper asked to take over after `since` (CLOCK_REALTIME).
int runtime_takeover_requested(struct timespec since);
