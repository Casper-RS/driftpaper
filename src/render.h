#pragma once
#include <GLES2/gl2.h>
#include <time.h>
#include "analyze.h"
#include "config.h"

#define MAX_IMAGES 8

struct background_uniforms {
    GLint tex, time, aspect, fit, ellipse, resolution, uv_scale, center, letterbox, viewport, opacity;
    GLint fog_a, fog_b, glow_a, glow_b, fog, glow, glow_falloff, pulse;
    GLint brightness, contrast, saturation, hue_adjust;
};

struct particle_uniforms {
    GLint time, aspect, radius_scale, inner, ellipse, motion, breathe, twinkle, resolution, center, viewport, opacity;
};

struct image {
    char path[PATH_LEN];
    struct timespec mtime;
    float ellipse;
    GLuint texture;
    int width, height, refs;
    struct analysis analysis;
};

struct renderer {
    GLuint background_program, particle_program, triangle_vbo;
    struct background_uniforms bg;
    struct particle_uniforms pt;
    struct image images[MAX_IMAGES];
};

struct scene {
    struct image *image;
    GLuint particle_vbo;
    int particle_count;
    struct config cfg;
    float center[2], inner_radius, outer_radius;
    float glow_a[3], glow_b[3];
};

// The part of the canvas one framebuffer shows, as fractions of the canvas. A single monitor
// shows all of it; with span, each monitor shows its own slice of one shared canvas.
struct viewport {
    float x, y, width, height;
};

#define FULL_VIEWPORT ((struct viewport){0, 0, 1, 1})

// All functions need a current GLES2 context. Images are cached and shared between scenes.
int renderer_init(struct renderer *r);
int scene_create(struct renderer *r, struct scene *s, const char *image_path, const struct config *cfg);
void scene_destroy(struct scene *s);
void render_draw(const struct renderer *r, const struct scene *s, int width, int height, struct viewport viewport,
                 float canvas_aspect, float time, float opacity);
