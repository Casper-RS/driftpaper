#define _GNU_SOURCE
#include "lockscreen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define COSMIC_BACKGROUND_DIR "/.config/cosmic/com.system76.CosmicBackground/v1"

int lockscreen_supported(void) {
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    return desktop && strstr(desktop, "COSMIC");
}

static const char *home(void) {
    const char *dir = getenv("HOME");
    return dir ? dir : "";
}

void lockscreen_image_path(const char *output, char *out, size_t size) {
    snprintf(out, size, "%s/.local/share/driftpaper/lockscreen-%s.png", home(), output);
}

static void make_directories(const char *path) {
    char partial[1024];
    snprintf(partial, sizeof partial, "%s", path);
    for (char *slash = partial + 1; (slash = strchr(slash, '/')); slash++) {
        *slash = 0;
        mkdir(partial, 0755);
        *slash = '/';
    }
    mkdir(partial, 0755);
}

static int write_atomically(const char *dir, const char *name, const char *contents) {
    char path[1200], temp[1210];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *f = fopen(temp, "w");
    if (!f) return -1;
    fputs(contents, f);
    fclose(f);
    return rename(temp, path);
}

static void background_entry(const char *output, const char *image, char *out, size_t size) {
    snprintf(out, size,
             "(\n    output: \"%s\",\n    source: Path(\"%s\"),\n    filter_by_theme: false,\n"
             "    rotation_frequency: 300,\n    filter_method: Lanczos,\n    scaling_mode: Zoom,\n"
             "    sampling_method: Alphanumeric,\n)",
             output, image);
}

void lockscreen_prepare(void) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/.local/share/driftpaper", home());
    make_directories(dir);
}

int lockscreen_apply(const char *const *outputs, int count) {
    if (count == 0) return 0;
    char dir[1024], image[1024], entry[2048], name[96], list[1024] = "[\n";
    snprintf(dir, sizeof dir, "%s%s", home(), COSMIC_BACKGROUND_DIR);
    make_directories(dir);

    int rc = 0;
    for (int i = 0; i < count; i++) {
        lockscreen_image_path(outputs[i], image, sizeof image);
        background_entry(outputs[i], image, entry, sizeof entry);
        snprintf(name, sizeof name, "output.%s", outputs[i]);
        rc |= write_atomically(dir, name, entry);
        size_t used = strlen(list);
        snprintf(list + used, sizeof list - used, "    \"%s\",\n", outputs[i]);
    }
    strncat(list, "]", sizeof list - strlen(list) - 1);

    lockscreen_image_path(outputs[0], image, sizeof image);
    background_entry("all", image, entry, sizeof entry);
    rc |= write_atomically(dir, "all", entry);
    rc |= write_atomically(dir, "backgrounds", list);
    rc |= write_atomically(dir, "same-on-all", "false");
    return rc;
}
