#define _GNU_SOURCE
#include "runtime.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *runtime_dir(void) {
    static char dir[512];
    const char *base = getenv("XDG_RUNTIME_DIR");
    const char *app = getenv("FLATPAK_ID");
    if (!base) base = "/tmp";
    // Each `flatpak run` gets its own /run/user/N view, but app/<id> is shared between them.
    if (app) snprintf(dir, sizeof dir, "%s/app/%s", base, app);
    else snprintf(dir, sizeof dir, "%s", base);
    return dir;
}

int runtime_find_wayland_display(void) {
    const char *current = getenv("WAYLAND_DISPLAY");
    if (current && *current) return 0;
    const char *base = getenv("XDG_RUNTIME_DIR");
    if (!base) return -1;
    for (int i = 0; i < 4; i++) {
        char name[16], path[600];
        snprintf(name, sizeof name, "wayland-%d", i);
        snprintf(path, sizeof path, "%s/%s", base, name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            setenv("WAYLAND_DISPLAY", name, 1);
            return 0;
        }
    }
    return -1;
}

static int open_lock_file(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/driftpaper.lock", runtime_dir());
    return open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
}

static struct flock whole_file_lock(short type) {
    return (struct flock){.l_type = type, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
}

int runtime_acquire_lock(void) {
    int fd = open_lock_file();
    if (fd < 0) return -2;
    struct flock lock = whole_file_lock(F_WRLCK);
    if (fcntl(fd, F_SETLK, &lock) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// F_GETLK only tests the lock, so checking never blocks a wallpaper that is starting up.
int runtime_wallpaper_running(void) {
    int fd = open_lock_file();
    if (fd < 0) return 0;
    struct flock lock = whole_file_lock(F_WRLCK);
    int locked = fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type != F_UNLCK;
    close(fd);
    return locked;
}
