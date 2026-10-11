#pragma once
#include "config.h"

struct settings_source {
    const char *config_path;
    int config_required;
    const char *preset;
    const char *image;
    const char *overrides[64];
    int n_overrides;
};

// Builds the effective config: defaults, preset, config file, image argument, then --set overrides.
int settings_load(const struct settings_source *src, struct config *out);
const char *settings_config_path(const struct settings_source *src);
