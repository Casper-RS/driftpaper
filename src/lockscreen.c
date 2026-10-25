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

static int same_contents(const char *path, const char *contents) {
    char buffer[4096];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t length = fread(buffer, 1, sizeof buffer, f);
    fclose(f);
    return length == strlen(contents) && !memcmp(buffer, contents, length);
}

// Returns 1 when the file was written, 0 when it already had these contents, -1 on failure.
static int write_if_changed(const char *dir, const char *name, const char *contents) {
    char path[1200], temp[1210];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (same_contents(path, contents)) return 0;
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *f = fopen(temp, "w");
    if (!f) return -1;
    fputs(contents, f);
    fclose(f);
    return rename(temp, path) ? -1 : 1;
}

static int merge_results(int a, int b) {
    return a < 0 || b < 0 ? -1 : a || b;
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

#define MAX_LISTED 16

static int compare_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Monitors COSMIC already lists keep their entry (and their last picture) while the animation is
// hidden on them, so showing or hiding it there doesn't make cosmic-bg reload.
static int read_listed(const char *dir, char names[MAX_LISTED][64]) {
    char path[1200], text[2048];
    snprintf(path, sizeof path, "%s/backgrounds", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t length = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[length] = 0;
    int n = 0;
    for (char *open = strchr(text, '"'); open && n < MAX_LISTED; open = strchr(open + 1, '"')) {
        char *close = strchr(open + 1, '"');
        if (!close) break;
        snprintf(names[n++], 64, "%.*s", (int)(close - open - 1), open + 1);
        open = close;
    }
    return n;
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
        rc = merge_results(rc, write_if_changed(dir, name, entry));
    }

    // Sorted, so the files come out the same whatever order the monitors were announced in.
    char listed[MAX_LISTED][64];
    const char *names[MAX_LISTED];
    int n = 0, n_listed = read_listed(dir, listed);
    for (int i = 0; i < count && n < MAX_LISTED; i++) names[n++] = outputs[i];
    for (int i = 0; i < n_listed && n < MAX_LISTED; i++) {
        int known = 0;
        for (int j = 0; j < n && !known; j++) known = !strcmp(names[j], listed[i]);
        if (!known) names[n++] = listed[i];
    }
    qsort(names, n, sizeof names[0], compare_names);
    for (int i = 0; i < n; i++) {
        size_t used = strlen(list);
        snprintf(list + used, sizeof list - used, "    \"%s\",\n", names[i]);
    }
    strncat(list, "]", sizeof list - strlen(list) - 1);

    const char *first = outputs[0];
    for (int i = 1; i < count; i++)
        if (strcmp(outputs[i], first) < 0) first = outputs[i];
    lockscreen_image_path(first, image, sizeof image);
    background_entry("all", image, entry, sizeof entry);
    rc = merge_results(rc, write_if_changed(dir, "all", entry));
    rc = merge_results(rc, write_if_changed(dir, "backgrounds", list));
    rc = merge_results(rc, write_if_changed(dir, "same-on-all", "false"));
    return rc;
}
