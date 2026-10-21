#define _GNU_SOURCE
#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *shape_names[SHAPE_COUNT] = {"triangle", "square", "spark", "circle", "ring", "diamond", "star", "plus"};
static const char *motion_names[] = {"orbit", "rise", "fall", "drift"};
static const char *dist_names[] = {"even", "outer", "inner"};
static const char *dir_names[] = {"both", "cw", "ccw"};
static const char *scale_names[] = {"fill", "fit", "stretch"};
static const char *renderer_names[] = {"auto", "gpu", "cpu"};
static const char *music_style_names[] = {"ring", "wave", "glow"};

static const struct { const char *name, *desc, *body; } presets[] = {
    {"shards", "orbiting triangles, squares and sparks around the subject (default)", ""},
    {"stardust", "dense field of tiny twinkling sparks orbiting the subject",
     "count=380\nshapes=spark:3,circle:1\nsize=4-11\nspeed=0.6\nspeed_falloff=3\ntwinkle=0.7\nwhite=0.5\ndistribution=outer\n"},
    {"geometric", "slow orbit of outlined shapes",
     "count=120\nshapes=ring:1,triangle:1,diamond:1,square:1\nsize=12-26\nspin=0.6\nwhite=0.15\ntwinkle=0.2\n"},
    {"embers", "warm sparks rising from the bottom of the screen",
     "motion=rise\ncount=200\nshapes=spark:3,circle:1\nsize=8-20\nopacity=0.5-1\ncolors=#ff7a2e,#ffb347,#ff4d2e\nwhite=0.1\ntwinkle=0.5\nfog=0.2\n"},
    {"snow", "soft flakes falling across the screen",
     "motion=fall\ncount=220\nshapes=circle:3,spark:1\nsize=4-12\ncolors=#ffffff,#dfe8ff\nwhite=0\ntwinkle=0.1\nspin=0.3\nopacity=0.35-0.85\n"},
    {"fireflies", "a few glowing dots wandering slowly",
     "motion=drift\ncount=60\nshapes=spark\nsize=10-24\ncolors=#d8ff6a,#ffe680\nwhite=0\ntwinkle=0.9\n"},
    {"confetti", "colourful tumbling shapes falling down",
     "motion=fall\ncount=160\nshapes=triangle:1,diamond:1,star:1,plus:1\nsize=8-16\nspin=3\nwhite=0\ntwinkle=0\nopacity=0.6-0.95\n"},
};
#define N_PRESETS (int)(sizeof presets / sizeof presets[0])

void config_defaults(struct config *c) {
    memset(c, 0, sizeof *c);
    c->enabled = 1;
    c->renderer = RENDERER_AUTO;
    c->count = 180;
    c->motion = MOTION_ORBIT;
    c->shape_weight[SHAPE_TRIANGLE] = 0.4f;
    c->shape_weight[SHAPE_SQUARE] = 0.28f;
    c->shape_weight[SHAPE_SPARK] = 0.32f;
    c->size_min = 9; c->size_max = 20;
    c->speed = 1; c->speed_falloff = 6; c->spin = 1;
    c->opacity_min = 0.3f; c->opacity_max = 0.8f;
    c->twinkle = 0.35f;
    c->breathe = 30;
    c->distribution = DIST_EVEN;
    c->direction = DIR_BOTH;
    c->inner_auto = c->center_auto = c->colors_auto = c->glow_auto = 1;
    c->inner_radius = 0.17f; c->outer_radius = 0.57f;
    c->center_x = 0.5f; c->center_y = 0.5f;
    c->ellipse = 1.45f;
    c->white = 0.3f;
    c->seed = 42;
    c->scale = SCALE_FILL;
    c->fog = 0.3f; c->glow = 1; c->pulse = 1;
    c->contrast = 1; c->saturation = 1;
    c->hue_center = 220; c->hue_width = 40;
    c->music_style = MUSIC_RING;
    c->music_badge = 1;
    c->music_sensitivity = 1;
    snprintf(c->music_source, sizeof c->music_source, "default");
    c->music_size = 1; c->music_brightness = 1; c->music_shadow = 0.4f;
    c->music_bars = 72;
    c->music_colors_auto = 1;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int bad(const char *where, const char *key, const char *value, const char *expect) {
    fprintf(stderr, "driftpaper: %s: invalid %s = '%s' (expected %s)\n", where, key, value, expect);
    return -1;
}

static int parse_float(const char *s, float *out) {
    char *end; errno = 0;
    float v = strtof(s, &end);
    if (end == s || errno || !isfinite(v)) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return -1;
    *out = v; return 0;
}

static int parse_range(const char *s, float *lo, float *hi) {
    char buf[64]; snprintf(buf, sizeof buf, "%s", s);
    if (!buf[0]) return -1;
    char *sep = strpbrk(buf + 1, "-,");
    if (!sep) { if (parse_float(buf, lo)) return -1; *hi = *lo; return 0; }
    *sep = 0;
    if (parse_float(trim(buf), lo) || parse_float(trim(sep + 1), hi)) return -1;
    if (*lo > *hi) { float t = *lo; *lo = *hi; *hi = t; }
    return 0;
}

static int parse_enum(const char *s, const char **names, int n) {
    for (int i = 0; i < n; i++) if (!strcasecmp(s, names[i])) return i;
    return -1;
}

static int parse_color(const char *s, float rgb[3]) {
    if (*s == '#') s++;
    unsigned v;
    if (strlen(s) != 6 || strspn(s, "0123456789abcdefABCDEF") != 6 || sscanf(s, "%6x", &v) != 1) return -1;
    rgb[0] = ((v >> 16) & 255) / 255.0f; rgb[1] = ((v >> 8) & 255) / 255.0f; rgb[2] = (v & 255) / 255.0f;
    return 0;
}

static int is_auto(const char *s) { return !strcasecmp(s, "auto"); }

static int parse_bool(const char *s, int *out) {
    if (!strcasecmp(s, "true") || !strcmp(s, "1") || !strcasecmp(s, "yes")) *out = 1;
    else if (!strcasecmp(s, "false") || !strcmp(s, "0") || !strcasecmp(s, "no")) *out = 0;
    else return -1;
    return 0;
}

static void expand_home(char dst[PATH_LEN], const char *path) {
    const char *home = getenv("HOME");
    if (path[0] == '~' && path[1] == '/') snprintf(dst, PATH_LEN, "%s%s", home ? home : "", path + 1);
    else snprintf(dst, PATH_LEN, "%s", path);
}

int config_set_output_image(struct config *c, const char *output, const char *image) {
    int i = 0;
    while (i < c->n_output_images && strcmp(c->output_images[i].output, output)) i++;
    if (!image || !*image) {
        if (i < c->n_output_images) c->output_images[i] = c->output_images[--c->n_output_images];
        return 0;
    }
    if (i == c->n_output_images) {
        if (i == MAX_OUTPUTS_FILTER) return -1;
        c->n_output_images++;
        snprintf(c->output_images[i].output, sizeof c->output_images[i].output, "%s", output);
    }
    expand_home(c->output_images[i].image, image);
    return 0;
}

const char *config_image_for(const struct config *c, const char *output) {
    for (int i = 0; output && i < c->n_output_images; i++)
        if (!strcmp(c->output_images[i].output, output)) return c->output_images[i].image;
    return c->image;
}

int config_set(struct config *c, const char *key, const char *value, const char *where) {
    char vbuf[PATH_LEN];
    snprintf(vbuf, sizeof vbuf, "%s", value);
    char *v = trim(vbuf);
    float f, g;
    int e;

    if (!strcmp(key, "preset")) return 0;
    if (!strcmp(key, "image")) {
        expand_home(c->image, v);
        return 0;
    }
    if (!strncmp(key, "image.", 6)) {
        if (!key[6] || strlen(key + 6) >= sizeof c->output_images[0].output) return bad(where, key, v, "image.<monitor name>");
        return config_set_output_image(c, key + 6, v);
    }
    if (!strcmp(key, "span")) return parse_bool(v, &c->span) ? bad(where, key, v, "true or false") : 0;
    if (!strcmp(key, "lock_screen_sync")) return parse_bool(v, &c->lock_screen_sync) ? bad(where, key, v, "true or false") : 0;
    if (!strcmp(key, "enabled")) return parse_bool(v, &c->enabled) ? bad(where, key, v, "true or false") : 0;
    if (!strcmp(key, "renderer")) {
        if ((e = parse_enum(v, renderer_names, 3)) < 0) return bad(where, key, v, "auto, gpu or cpu");
        c->renderer = e; return 0;
    }
    if (!strcmp(key, "fps")) {
        if (is_auto(v)) { c->fps = 0; return 0; }
        if (parse_float(v, &f) || f < 1 || f > 240) return bad(where, key, v, "auto or 1-240");
        c->fps = (int)f; return 0;
    }
    if (!strcmp(key, "render_scale")) {
        if (is_auto(v)) { c->render_scale = 0; return 0; }
        if (parse_float(v, &f) || f < 0.25f || f > 1) return bad(where, key, v, "auto or 0.25-1");
        c->render_scale = f; return 0;
    }
    if (!strcmp(key, "outputs")) {
        c->n_outputs = 0;
        if (!strcasecmp(v, "all")) return 0;
        for (char *save, *tok = strtok_r(v, ",", &save); tok && c->n_outputs < MAX_OUTPUTS_FILTER; tok = strtok_r(NULL, ",", &save))
            snprintf(c->outputs[c->n_outputs++], sizeof c->outputs[0], "%s", trim(tok));
        return 0;
    }
    if (!strcmp(key, "count")) {
        if (parse_float(v, &f) || f < 0 || f > 20000) return bad(where, key, v, "0-20000");
        c->count = (int)f; return 0;
    }
    if (!strcmp(key, "motion")) {
        if ((e = parse_enum(v, motion_names, 4)) < 0) return bad(where, key, v, "orbit, rise, fall or drift");
        c->motion = e; return 0;
    }
    if (!strcmp(key, "shapes")) {
        float w[SHAPE_COUNT] = {0};
        for (char *save, *tok = strtok_r(v, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            tok = trim(tok);
            char *colon = strchr(tok, ':');
            float wt = 1;
            if (colon) { *colon = 0; if (parse_float(trim(colon + 1), &wt) || wt < 0) return bad(where, key, v, "name[:weight], ..."); }
            if ((e = parse_enum(trim(tok), shape_names, SHAPE_COUNT)) < 0) {
                fprintf(stderr, "driftpaper: %s: unknown shape '%s' (see --list)\n", where, tok);
                return -1;
            }
            w[e] += wt;
        }
        memcpy(c->shape_weight, w, sizeof w);
        return 0;
    }
    if (!strcmp(key, "size")) {
        if (parse_range(v, &f, &g) || f <= 0) return bad(where, key, v, "min-max in px");
        c->size_min = f; c->size_max = g; return 0;
    }
    if (!strcmp(key, "opacity")) {
        if (parse_range(v, &f, &g) || f < 0 || g > 1) return bad(where, key, v, "min-max between 0 and 1");
        c->opacity_min = f; c->opacity_max = g; return 0;
    }
    struct { const char *k; float *p; float lo, hi; } nums[] = {
        {"speed", &c->speed, 0, 100}, {"speed_falloff", &c->speed_falloff, 0.1f, 100}, {"spin", &c->spin, 0, 100},
        {"twinkle", &c->twinkle, 0, 1}, {"breathe", &c->breathe, 0, 500}, {"outer_radius", &c->outer_radius, 0, 10},
        {"ellipse", &c->ellipse, 0.1f, 10}, {"white", &c->white, 0, 1}, {"fog", &c->fog, 0, 5}, {"glow", &c->glow, 0, 10},
        {"pulse", &c->pulse, 0, 20}, {"brightness", &c->brightness, -1, 1}, {"contrast", &c->contrast, 0, 4},
        {"saturation", &c->saturation, 0, 4}, {"music_sensitivity", &c->music_sensitivity, 0.1f, 5},
        {"music_size", &c->music_size, 0.5f, 2}, {"music_brightness", &c->music_brightness, 0, 3},
        {"music_shadow", &c->music_shadow, 0, 1},
    };
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        if (strcmp(key, nums[i].k)) continue;
        if (parse_float(v, &f) || f < nums[i].lo || f > nums[i].hi) {
            char r[64]; snprintf(r, sizeof r, "a number from %g to %g", nums[i].lo, nums[i].hi);
            return bad(where, key, v, r);
        }
        *nums[i].p = f; return 0;
    }
    if (!strcmp(key, "seed")) {
        if (parse_float(v, &f) || f < 0 || f >= 4294967296.0f) return bad(where, key, v, "a whole number from 0 to 4294967295");
        c->seed = (unsigned)f; return 0;
    }
    if (!strcmp(key, "distribution")) {
        if ((e = parse_enum(v, dist_names, 3)) < 0) return bad(where, key, v, "even, outer or inner");
        c->distribution = e; return 0;
    }
    if (!strcmp(key, "direction")) {
        if ((e = parse_enum(v, dir_names, 3)) < 0) return bad(where, key, v, "both, cw or ccw");
        c->direction = e; return 0;
    }
    if (!strcmp(key, "inner_radius")) {
        if (is_auto(v)) { c->inner_auto = 1; return 0; }
        if (parse_float(v, &f) || f < 0 || f > 10) return bad(where, key, v, "auto or a fraction of the image height");
        c->inner_auto = 0; c->inner_radius = f; return 0;
    }
    if (!strcmp(key, "center")) {
        if (is_auto(v)) { c->center_auto = 1; return 0; }
        if (sscanf(v, "%f , %f", &f, &g) != 2 || !isfinite(f) || !isfinite(g)) return bad(where, key, v, "auto or x,y");
        c->center_auto = 0; c->center_x = f; c->center_y = g; return 0;
    }
    if (!strcmp(key, "colors")) {
        if (is_auto(v)) { c->colors_auto = 1; return 0; }
        int n = 0; float cols[MAX_COLORS][3];
        for (char *save, *tok = strtok_r(v, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (n == MAX_COLORS) break;
            if (parse_color(trim(tok), cols[n])) return bad(where, key, tok, "auto or #rrggbb, ...");
            n++;
        }
        if (!n) return bad(where, key, v, "auto or #rrggbb, ...");
        memcpy(c->colors, cols, sizeof cols); c->n_colors = n; c->colors_auto = 0;
        return 0;
    }
    if (!strcmp(key, "music")) return parse_bool(v, &c->music) ? bad(where, key, v, "true or false") : 0;
    if (!strcmp(key, "music_badge")) return parse_bool(v, &c->music_badge) ? bad(where, key, v, "true or false") : 0;
    if (!strcmp(key, "music_style")) {
        if ((e = parse_enum(v, music_style_names, 3)) < 0) return bad(where, key, v, "ring, wave or glow");
        c->music_style = e; return 0;
    }
    if (!strcmp(key, "music_bars")) {
        if (parse_float(v, &f) || f < 24 || f > 144) return bad(where, key, v, "24-144");
        c->music_bars = (int)f; return 0;
    }
    if (!strcmp(key, "music_colors")) {
        if (is_auto(v)) { c->music_colors_auto = 1; return 0; }
        int n = 0; float cols[2][3];
        for (char *save, *tok = strtok_r(v, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (n == 2 || parse_color(trim(tok), cols[n])) return bad(where, key, tok, "auto or one or two #rrggbb");
            n++;
        }
        if (!n) return bad(where, key, v, "auto or one or two #rrggbb");
        if (n == 1) memcpy(cols[1], cols[0], sizeof cols[1]);
        memcpy(c->music_colors, cols, sizeof cols); c->music_colors_auto = 0;
        return 0;
    }
    if (!strcmp(key, "music_source")) {
        if (!*v || strlen(v) >= sizeof c->music_source) return bad(where, key, v, "default or a source name");
        snprintf(c->music_source, sizeof c->music_source, "%s", v);
        return 0;
    }
    if (!strcmp(key, "scale")) {
        if ((e = parse_enum(v, scale_names, 3)) < 0) return bad(where, key, v, "fill, fit or stretch");
        c->scale = e; return 0;
    }
    if (!strcmp(key, "bg_color")) {
        if (parse_color(v, c->bg_color)) return bad(where, key, v, "#rrggbb");
        return 0;
    }
    if (!strcmp(key, "glow_color")) {
        if (is_auto(v)) { c->glow_auto = 1; return 0; }
        if (parse_color(v, c->glow_color)) return bad(where, key, v, "auto or #rrggbb");
        c->glow_auto = 0; return 0;
    }
    if (!strcmp(key, "hue_adjust")) {
        if (!strcasecmp(v, "off")) { c->hue_lightness = 0; return 0; }
        float hue, width, lightness;
        if (sscanf(v, "%f , %f , %f", &hue, &width, &lightness) != 3 || !isfinite(hue) || !isfinite(width) ||
            !isfinite(lightness))
            return bad(where, key, v, "off or hue,width,lightness (e.g. 220,40,-0.2)");
        c->hue_center = hue; c->hue_width = width; c->hue_lightness = lightness;
        return 0;
    }
    fprintf(stderr, "driftpaper: %s: unknown setting '%s'\n", where, key);
    return -1;
}

int config_apply_preset(struct config *c, const char *name) {
    for (int i = 0; i < N_PRESETS; i++) {
        if (strcasecmp(name, presets[i].name)) continue;
        char *body = strdup(presets[i].body);
        int rc = 0;
        for (char *save, *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *eq = strchr(line, '=');
            *eq = 0;
            rc |= config_set(c, line, eq + 1, "preset");
        }
        free(body);
        return rc;
    }
    fprintf(stderr, "driftpaper: unknown preset '%s' (see --list)\n", name);
    return -1;
}

static int each_line(const char *path, int (*fn)(void *, char *, char *, int), void *ud) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[PATH_LEN + 64];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) { fprintf(stderr, "driftpaper: %s:%d: expected key = value\n", path, lineno); rc = -1; continue; }
        *eq = 0;
        rc |= fn(ud, trim(s), trim(eq + 1), lineno);
    }
    fclose(f);
    return rc;
}

static void make_relative_to(const char *config_path, char image[PATH_LEN]) {
    const char *slash = strrchr(config_path, '/');
    char joined[2 * PATH_LEN];
    if (!image[0] || image[0] == '/' || !slash) return;
    if (snprintf(joined, sizeof joined, "%.*s/%s", (int)(slash - config_path), config_path, image) < PATH_LEN)
        memcpy(image, joined, PATH_LEN);
}

struct load_ctx { struct config *c; const char *path; char preset[64]; };

static int find_preset(void *ud, char *k, char *v, int lineno) {
    (void)lineno;
    struct load_ctx *l = ud;
    if (!strcmp(k, "preset")) snprintf(l->preset, sizeof l->preset, "%s", v);
    return 0;
}

static int apply_line(void *ud, char *k, char *v, int lineno) {
    struct load_ctx *l = ud;
    char where[PATH_LEN + 16];
    snprintf(where, sizeof where, "%s:%d", l->path, lineno);
    return config_set(l->c, k, v, where);
}

int config_load(struct config *c, const char *path, const char *preset_override) {
    config_defaults(c);
    struct load_ctx l = {.c = c, .path = path};
    int have_file = path && each_line(path, find_preset, &l) == 0;
    if (preset_override) snprintf(l.preset, sizeof l.preset, "%s", preset_override);
    int rc = 0;
    if (l.preset[0]) rc |= config_apply_preset(c, l.preset);
    if (have_file) {
        rc |= each_line(path, apply_line, &l);
        make_relative_to(path, c->image);
        for (int i = 0; i < c->n_output_images; i++) make_relative_to(path, c->output_images[i].image);
    }
    return rc;
}

static void print_color(FILE *f, const float *rgb) {
    fprintf(f, "#%02x%02x%02x", (int)(rgb[0] * 255 + 0.5f), (int)(rgb[1] * 255 + 0.5f), (int)(rgb[2] * 255 + 0.5f));
}

void config_print(const struct config *c, FILE *f) {
    fprintf(f, "[general]\n");
    fprintf(f, "enabled = %s\n", c->enabled ? "true" : "false");
    fprintf(f, "image = %s\n", c->image);
    for (int i = 0; i < c->n_output_images; i++)
        fprintf(f, "image.%s = %s\n", c->output_images[i].output, c->output_images[i].image);
    fprintf(f, "span = %s\n", c->span ? "true" : "false");
    fprintf(f, "lock_screen_sync = %s\n", c->lock_screen_sync ? "true" : "false");
    fprintf(f, "renderer = %s\n", renderer_names[c->renderer]);
    if (c->fps) fprintf(f, "fps = %d\n", c->fps); else fprintf(f, "fps = auto\n");
    if (c->render_scale > 0) fprintf(f, "render_scale = %g\n", c->render_scale); else fprintf(f, "render_scale = auto\n");
    fprintf(f, "outputs = ");
    if (!c->n_outputs) fprintf(f, "all");
    for (int i = 0; i < c->n_outputs; i++) fprintf(f, "%s%s", i ? ", " : "", c->outputs[i]);
    fprintf(f, "\n\n[particles]\n");
    fprintf(f, "count = %d\n", c->count);
    fprintf(f, "motion = %s\n", motion_names[c->motion]);
    fprintf(f, "shapes = ");
    int first = 1;
    for (int i = 0; i < SHAPE_COUNT; i++)
        if (c->shape_weight[i] > 0) { fprintf(f, "%s%s:%g", first ? "" : ", ", shape_names[i], c->shape_weight[i]); first = 0; }
    fprintf(f, "\nsize = %g-%g\n", c->size_min, c->size_max);
    fprintf(f, "opacity = %g-%g\n", c->opacity_min, c->opacity_max);
    fprintf(f, "speed = %g\nspeed_falloff = %g\nspin = %g\ntwinkle = %g\nbreathe = %g\n", c->speed, c->speed_falloff, c->spin, c->twinkle, c->breathe);
    fprintf(f, "distribution = %s\ndirection = %s\n", dist_names[c->distribution], dir_names[c->direction]);
    if (c->inner_auto) fprintf(f, "inner_radius = auto\n"); else fprintf(f, "inner_radius = %g\n", c->inner_radius);
    fprintf(f, "outer_radius = %g\n", c->outer_radius);
    if (c->center_auto) fprintf(f, "center = auto\n"); else fprintf(f, "center = %g, %g\n", c->center_x, c->center_y);
    fprintf(f, "ellipse = %g\n", c->ellipse);
    fprintf(f, "colors = ");
    if (c->colors_auto) fprintf(f, "auto");
    for (int i = 0; !c->colors_auto && i < c->n_colors; i++) { if (i) fprintf(f, ", "); print_color(f, c->colors[i]); }
    fprintf(f, "\nwhite = %g\nseed = %u\n", c->white, c->seed);
    fprintf(f, "\n[background]\n");
    fprintf(f, "scale = %s\nbg_color = ", scale_names[c->scale]);
    print_color(f, c->bg_color);
    fprintf(f, "\nfog = %g\nglow = %g\nglow_color = ", c->fog, c->glow);
    if (c->glow_auto) fprintf(f, "auto"); else print_color(f, c->glow_color);
    fprintf(f, "\npulse = %g\nbrightness = %g\ncontrast = %g\nsaturation = %g\n", c->pulse, c->brightness, c->contrast, c->saturation);
    if (c->hue_lightness == 0) fprintf(f, "hue_adjust = off\n");
    else fprintf(f, "hue_adjust = %g, %g, %g\n", c->hue_center, c->hue_width, c->hue_lightness);
    fprintf(f, "\n[music]\n");
    fprintf(f, "music = %s\nmusic_style = %s\n", c->music ? "true" : "false", music_style_names[c->music_style]);
    fprintf(f, "music_badge = %s\nmusic_sensitivity = %g\nmusic_source = %s\n", c->music_badge ? "true" : "false",
            c->music_sensitivity, c->music_source);
    fprintf(f, "music_size = %g\nmusic_brightness = %g\nmusic_shadow = %g\nmusic_bars = %d\nmusic_colors = ",
            c->music_size, c->music_brightness, c->music_shadow, c->music_bars);
    if (c->music_colors_auto) fprintf(f, "auto");
    else { print_color(f, c->music_colors[0]); fprintf(f, ", "); print_color(f, c->music_colors[1]); }
    fprintf(f, "\n");
}

void config_list_presets(FILE *f) {
    fprintf(f, "Presets:\n");
    for (int i = 0; i < N_PRESETS; i++) fprintf(f, "  %-10s %s\n", presets[i].name, presets[i].desc);
    fprintf(f, "\nShapes:\n ");
    for (int i = 0; i < SHAPE_COUNT; i++) fprintf(f, " %s", shape_names[i]);
    fprintf(f, "\n\nMotions:\n  orbit rise fall drift\n");
}

int config_preset_count(void) { return N_PRESETS; }
const char *config_preset_name(int index) { return presets[index].name; }
const char *config_preset_description(int index) { return presets[index].desc; }

const char *config_default_path(void) {
    static char path[PATH_LEN];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(path, sizeof path, "%s/driftpaper/config.ini", xdg);
    else snprintf(path, sizeof path, "%s/.config/driftpaper/config.ini", getenv("HOME") ? getenv("HOME") : ".");
    return path;
}

int config_fps(const struct config *c) {
    if (c->fps) return c->fps;
    return c->renderer == RENDERER_CPU ? 12 : 30;
}

float config_render_scale(const struct config *c) {
    if (c->render_scale > 0) return c->render_scale;
    return c->renderer == RENDERER_CPU ? 0.35f : 1.0f;
}
