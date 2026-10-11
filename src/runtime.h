#pragma once

// Sandboxes such as Flatpak may hide WAYLAND_DISPLAY while still exposing the compositor's
// socket; this finds the socket and sets WAYLAND_DISPLAY. Returns 0 when one is available.
int runtime_find_wayland_display(void);

// Takes the single-instance lock. Returns a file descriptor to keep open, or -1 if another
// wallpaper already holds it.
int runtime_acquire_lock(void);
int runtime_wallpaper_running(void);
