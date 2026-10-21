#define _GNU_SOURCE
#include "runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        char name[24], path[600];
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

static void runtime_file(const char *name, char *out, size_t size) {
    snprintf(out, size, "%s/%s", runtime_dir(), name);
}

static int open_lock_file(void) {
    char path[600];
    runtime_file("driftpaper.lock", path, sizeof path);
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

#define TAKEOVER_WAIT_MS 5000
#define TAKEOVER_POLL_MS 100

int runtime_take_over(void) {
    char path[600];
    runtime_file("driftpaper.takeover", path, sizeof path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        futimens(fd, NULL);
        close(fd);
    }
    for (int waited = 0; waited < TAKEOVER_WAIT_MS; waited += TAKEOVER_POLL_MS) {
        int lock = runtime_acquire_lock();
        if (lock != -1) return lock;
        usleep(TAKEOVER_POLL_MS * 1000);
    }
    return -1;
}

int runtime_takeover_requested(struct timespec since) {
    char path[600];
    runtime_file("driftpaper.takeover", path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtim.tv_sec > since.tv_sec ||
           (st.st_mtim.tv_sec == since.tv_sec && st.st_mtim.tv_nsec > since.tv_nsec);
}

static int is_cosmic_bg(pid_t pid) {
    char path[64], comm[32] = "";
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
    fclose(f);
    return !strcmp(comm, "cosmic-bg\n");
}

pid_t runtime_cosmic_bg_pid(pid_t known) {
    if (known && is_cosmic_bg(known)) return known;
    DIR *proc = opendir("/proc");
    if (!proc) return 0;
    pid_t found = 0;
    for (struct dirent *e; !found && (e = readdir(proc));)
        if (isdigit((unsigned char)e->d_name[0]) && is_cosmic_bg(atoi(e->d_name))) found = atoi(e->d_name);
    closedir(proc);
    return found;
}
