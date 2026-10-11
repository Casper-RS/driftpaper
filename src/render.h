#pragma once
#include <GLES2/gl2.h>
#include "analyze.h"
#include "config.h"

struct background_uniforms {
    GLint tex, time, aspect, fit, ellipse, resolution, uv_scale, center, letterbox;
    GLint fog_a, fog_b, glow_a, glow_b, fog, glow, glow_falloff, pulse;
    GLint brightness, contrast, saturation, hue_adjust;
};

struct particle_uniforms {
    GLint time, aspect, radius_scale, inner, ellipse, motion, breathe, twinkle, resolution, center;
};

struct scene {
    GLuint background_program, particle_program, texture, triangle_vbo, particle_vbo;
    struct background_uniforms bg;
    struct particle_uniforms pt;
    int image_width, image_height, particle_count;
    struct analysis analysis;
    struct config cfg;
    float center[2], inner_radius, outer_radius;
    float glow_a[3], glow_b[3];
};

// All functions need a current GLES2 context.
int render_init(struct scene *s);
int render_load_image(struct scene *s, const char *path, float ellipse);
void render_apply_config(struct scene *s, const struct config *cfg);
void render_draw(const struct scene *s, int width, int height, float time);
