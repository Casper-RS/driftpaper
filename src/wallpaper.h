#pragma once
#include "settings.h"

// The config file and image are watched and reloaded when they change.
int wallpaper_run(const struct settings_source *src, const struct config *initial);
