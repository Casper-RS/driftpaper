#pragma once
#include "config.h"
#include "render.h"

#define SNAPSHOT_TIME 20.0f

// Renders one frame of `s` offscreen, without the music visualiser (it is used for lock screens),
// as RGBA rows from the bottom up (as glReadPixels gives them) for snapshot_save_png; NULL on
// failure. Needs a current GLES2 context.
unsigned char *snapshot_render_rgba(const struct renderer *r, const struct scene *s, int width, int height,
                                    struct viewport viewport, float canvas_aspect);
// Needs no GL context, so it can run on another thread. Replaces `path` only once the PNG is complete.
int snapshot_save_png(const char *path, int width, int height, const unsigned char *rgba);

// Stand-alone export for the command line: sets up its own context. With `music`, the visualiser
// is drawn for a sample spectrum (when music sync is on in `cfg`).
int snapshot_write(const struct config *cfg, const char *path, int width, int height, float time, int music);
