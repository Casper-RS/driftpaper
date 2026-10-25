#pragma once
#include <GLES2/gl2.h>
#include <time.h>
#include "analyze.h"
#include "config.h"
#include "music.h"

#define MAX_IMAGES 8

struct background_uniforms {
    GLint tex, time, aspect, fit, ellipse, resolution, uv_scale, center, letterbox, viewport, opacity;
    GLint fog_a, fog_b, glow_a, glow_b, fog, glow, glow_falloff, pulse;
    GLint brightness, contrast, saturation, hue_adjust, music;
};

struct particle_uniforms {
    GLint time, aspect, radius_scale, inner, ellipse, motion, breathe, twinkle, resolution, center, viewport, opacity, music;
};

struct music_uniforms {
    GLint mode, time, aspect, resolution, viewport, center, radius, bands, beat, bass, presence, color_a, color_b, badge, opacity;
    GLint size, bars, shadow, gain, beat_ages, badge_style;
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
    GLuint background_program, particle_program, music_program, triangle_vbo;
    struct background_uniforms bg;
    struct particle_uniforms pt;
    struct music_uniforms mu;
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

// A picture decoded and analysed away from the GL thread, ready for renderer_add_image.
struct decoded_image {
    char path[PATH_LEN];
    float ellipse;
    struct timespec mtime;
    unsigned char *pixels;
    int width, height;
    struct analysis analysis;
};

// Needs no GL context, so it can run on another thread.
int image_decode(struct decoded_image *d, const char *path, float ellipse);
void image_decoded_free(struct decoded_image *d);

// All functions below need a current GLES2 context. Images are cached and shared between scenes.
int renderer_init(struct renderer *r);
// Whether `path`, as it is on disk now, is loaded for this ellipse.
int renderer_has_image(struct renderer *r, const char *path, float ellipse);
// Uploads a decoded picture (freeing its pixels) and holds it until renderer_release_image.
// Returns NULL when it can't be used or every slot is taken.
struct image *renderer_add_image(struct renderer *r, struct decoded_image *d);
void renderer_release_image(struct image *img);
int scene_create(struct renderer *r, struct scene *s, const char *image_path, const struct config *cfg);
void scene_destroy(struct scene *s);
// `pulse` (0 for none, about 1 on a strong beat) makes the glow and particles throb with the music.
void render_draw(const struct renderer *r, const struct scene *s, int width, int height, struct viewport viewport,
                 float canvas_aspect, float time, float opacity, float pulse);
// Strength of render_draw's pulse for the current music.
float render_music_pulse(const struct config *cfg, const struct music_levels *m);
// Draws the music visualiser of `s`'s style over a frame from render_draw, and with `badge` the
// small round badge of `s`'s badge style in the bottom-right corner.
void render_music(const struct renderer *r, const struct scene *s, int width, int height, struct viewport viewport,
                  float canvas_aspect, float time, const struct music_levels *m, int badge, float opacity);
