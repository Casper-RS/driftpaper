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
enum renderer { RENDERER_AUTO, RENDERER_GPU, RENDERER_CPU };

extern const char *shape_names[SHAPE_COUNT];

struct config {
    char image[PATH_LEN];
    enum renderer renderer;
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
};

void config_defaults(struct config *c);
int config_apply_preset(struct config *c, const char *name);
int config_set(struct config *c, const char *key, const char *value, const char *where);
// Layers defaults, then the preset (override or the file's), then the file's other keys.
int config_load(struct config *c, const char *path, const char *preset_override);
void config_print(const struct config *c, FILE *f);
void config_list_presets(FILE *f);
const char *config_default_path(void);

// fps and render_scale of 0 mean "auto": full quality on a GPU, reduced on the CPU renderer.
int config_fps(const struct config *c);
float config_render_scale(const struct config *c);
