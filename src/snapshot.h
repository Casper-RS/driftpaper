#pragma once
#include "config.h"
#include "render.h"

#define SNAPSHOT_TIME 20.0f

// Renders one frame of `s` offscreen and saves it as a PNG, without the music visualiser (it is
// used for lock screens). Needs a current GLES2 context.
int snapshot_render_png(const struct renderer *r, const struct scene *s, int width, int height,
                        struct viewport viewport, float canvas_aspect, const char *path);

// Stand-alone export for the command line: sets up its own context. With `music`, the visualiser
// is drawn for a sample spectrum (when music sync is on in `cfg`).
int snapshot_write(const struct config *cfg, const char *path, int width, int height, float time, int music);
