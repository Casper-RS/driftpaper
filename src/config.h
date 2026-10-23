#pragma once
#include <stdio.h>

#define MAX_COLORS 8
#define MAX_OUTPUTS_FILTER 8
#define PATH_LEN 1024

enum shape { SHAPE_TRIANGLE, SHAPE_SQUARE, SHAPE_SPARK, SHAPE_CIRCLE, SHAPE_RING, SHAPE_DIAMOND, SHAPE_STAR, SHAPE_PLUS, SHAPE_COUNT };
enum motion { MOTION_ORBIT, MOTION_RISE, MOTION_FALL, MOTION_DRIFT };
enum distribution { DIST_EVEN, DIST_OUTER, DIST_INNER };
enum direction { DIR_BOTH, DIR_CW, DIR_CCW };
enum scale_mode { SCALE_FILL, SCALE_FIT, SCALE_STRETCH };
enum renderer_kind { RENDERER_AUTO, RENDERER_GPU, RENDERER_CPU };
enum music_style { MUSIC_RING, MUSIC_HALO, MUSIC_RIPPLES, MUSIC_GLOW };
enum music_badge { BADGE_OFF, BADGE_BARS, BADGE_DIAL, BADGE_VINYL };

extern const char *shape_names[SHAPE_COUNT];

struct output_image {
    char output[32];
    char image[PATH_LEN];
};

struct config {
    char image[PATH_LEN];
    struct output_image output_images[MAX_OUTPUTS_FILTER];
    int n_output_images;
    int span;
    int lock_screen_sync;
    int enabled;
    enum renderer_kind renderer;
    int fps;
    float render_scale;
    char outputs[MAX_OUTPUTS_FILTER][32];
    int n_outputs;

    // Sizes are pixels at 1440p; radii are fractions of the displayed image height.
    int count;
    enum motion motion;
    float shape_weight[SHAPE_COUNT];
    float size_min, size_max;
    float speed;
    float speed_falloff;
    float spin;
    float opacity_min, opacity_max;
    float twinkle;
    float breathe;
    enum distribution distribution;
    enum direction direction;
    int inner_auto, center_auto, colors_auto;
    float inner_radius, outer_radius;
    float center_x, center_y;
    float ellipse;
    float colors[MAX_COLORS][3];
    int n_colors;
    float white;
    unsigned seed;

    enum scale_mode scale;
    float bg_color[3];
    float fog, glow, pulse;
    int glow_auto;
    float glow_color[3];
    float brightness, contrast, saturation;
    float hue_center, hue_width, hue_lightness;

    // Music sync: the look reacts to what the speakers play.
    int music;
    enum music_style music_style;
    enum music_badge music_badge;
    float music_sensitivity;
    char music_source[256];
    float music_size, music_brightness, music_shadow;
    int music_bars;
    int music_colors_auto;
    float music_colors[2][3];
};

void config_defaults(struct config *c);
int config_apply_preset(struct config *c, const char *name);
int config_set(struct config *c, const char *key, const char *value, const char *where);
// Layers defaults, then the preset (override or the file's), then the file's other keys.
int config_load(struct config *c, const char *path, const char *preset_override);
void config_print(const struct config *c, FILE *f);
void config_list_presets(FILE *f);
int config_preset_count(void);
const char *config_preset_name(int index);
const char *config_preset_description(int index);
const char *config_default_path(void);
// The picture for a monitor: its own image.<name> if set, otherwise the main image.
const char *config_image_for(const struct config *c, const char *output);
int config_set_output_image(struct config *c, const char *output, const char *image);

// fps and render_scale of 0 mean "auto": full quality on a GPU, reduced on the CPU renderer.
int config_fps(const struct config *c);
float config_render_scale(const struct config *c);
