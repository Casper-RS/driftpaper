#include "settings.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

const char *settings_config_path(const struct settings_source *src) {
    return src->config_path ? src->config_path : config_default_path();
}

static int apply_override(struct config *cfg, const char *assignment) {
    const char *equals = strchr(assignment, '=');
    if (!equals) {
        fprintf(stderr, "driftpaper: --set expects key=value, got '%s'\n", assignment);
        return -1;
    }
    char key[128];
    snprintf(key, sizeof key, "%.*s", (int)(equals - assignment), assignment);
    return config_set(cfg, key, equals + 1, "--set");
}

int settings_load(const struct settings_source *src, struct config *out) {
    const char *path = settings_config_path(src);
    int readable = access(path, R_OK) == 0;
    if (src->config_required && !readable) {
        fprintf(stderr, "driftpaper: cannot read config '%s': %s\n", path, strerror(errno));
        return -1;
    }

    int rc = config_load(out, readable ? path : NULL, src->preset);
    if (src->image) rc |= config_set(out, "image", src->image, "command line");
    for (int i = 0; i < src->n_overrides; i++) rc |= apply_override(out, src->overrides[i]);
    return rc;
}
