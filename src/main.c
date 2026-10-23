#define _GNU_SOURCE
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "analyze.h"
#include "egl.h"
#include "settings.h"
#include "snapshot.h"
#include "stb_image.h"
#include "wallpaper.h"

#ifndef VERSION
#define VERSION "dev"
#endif

enum mode { MODE_WALLPAPER, MODE_PRINT_CONFIG, MODE_ANALYZE, MODE_SNAPSHOT };

struct options {
    enum mode mode;
    struct settings_source src;
    const char *snapshot_path;
    int snapshot_width, snapshot_height;
    float snapshot_time;
    int snapshot_music;
};

static void print_usage(FILE *out) {
    fprintf(out,
            "Usage: driftpaper [options] [IMAGE]\n"
            "\n"
            "Animated particle wallpaper for Wayland. Give it any image; it finds the subject,\n"
            "picks colours from it and animates particles around it.\n"
            "\n"
            "  -c, --config FILE      config file (default: %s)\n"
            "  -p, --preset NAME      start from a preset (see --list)\n"
            "  -s, --set KEY=VALUE    override one setting, e.g. -s count=300 (repeatable)\n"
            "  -l, --list             list presets, shapes and motions\n"
            "      --print-config     print the effective settings as a config file\n"
            "      --analyze          show what was detected in the image\n"
            "      --snapshot FILE    render one frame to a PNG (e.g. for the lock screen) and exit\n"
            "      --size WxH         snapshot size (default 2560x1440)\n"
            "      --time SECONDS     animation time of the snapshot (default 20)\n"
            "      --with-music       show the music visualiser in the snapshot, on a sample song\n"
            "  -h, --help             show this help\n"
            "  -V, --version          show the version\n"
            "\n"
            "The config file is watched while running and changes apply live.\n",
            config_default_path());
}

static int parse_options(int argc, char **argv, struct options *opt) {
    enum { OPT_PRINT_CONFIG = 256, OPT_ANALYZE, OPT_SNAPSHOT, OPT_SIZE, OPT_TIME, OPT_WITH_MUSIC };
    static const struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"preset", required_argument, 0, 'p'},
        {"set", required_argument, 0, 's'},
        {"list", no_argument, 0, 'l'},
        {"print-config", no_argument, 0, OPT_PRINT_CONFIG},
        {"analyze", no_argument, 0, OPT_ANALYZE},
        {"snapshot", required_argument, 0, OPT_SNAPSHOT},
        {"size", required_argument, 0, OPT_SIZE},
        {"time", required_argument, 0, OPT_TIME},
        {"with-music", no_argument, 0, OPT_WITH_MUSIC},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0},
    };
    *opt = (struct options){.snapshot_width = 2560, .snapshot_height = 1440, .snapshot_time = 20};

    int c;
    while ((c = getopt_long(argc, argv, "c:p:s:lhV", long_options, NULL)) != -1) {
        switch (c) {
        case 'c':
            opt->src.config_path = optarg;
            opt->src.config_required = 1;
            break;
        case 'p':
            opt->src.preset = optarg;
            break;
        case 's':
            if (opt->src.n_overrides == (int)(sizeof opt->src.overrides / sizeof opt->src.overrides[0])) {
                fprintf(stderr, "driftpaper: too many --set options\n");
                return 2;
            }
            opt->src.overrides[opt->src.n_overrides++] = optarg;
            break;
        case 'l':
            config_list_presets(stdout);
            exit(0);
        case OPT_PRINT_CONFIG:
            opt->mode = MODE_PRINT_CONFIG;
            break;
        case OPT_ANALYZE:
            opt->mode = MODE_ANALYZE;
            break;
        case OPT_SNAPSHOT:
            opt->mode = MODE_SNAPSHOT;
            opt->snapshot_path = optarg;
            break;
        case OPT_SIZE:
            if (sscanf(optarg, "%dx%d", &opt->snapshot_width, &opt->snapshot_height) != 2 ||
                opt->snapshot_width < 16 || opt->snapshot_height < 16 ||
                opt->snapshot_width > 16384 || opt->snapshot_height > 16384) {
                fprintf(stderr, "driftpaper: --size expects WxH, e.g. 3840x2160\n");
                return 2;
            }
            break;
        case OPT_TIME: {
            char *end;
            opt->snapshot_time = strtof(optarg, &end);
            if (end == optarg || *end || !isfinite(opt->snapshot_time) || opt->snapshot_time < 0) {
                fprintf(stderr, "driftpaper: --time expects a number of seconds\n");
                return 2;
            }
            break;
        }
        case OPT_WITH_MUSIC:
            opt->snapshot_music = 1;
            break;
        case 'h':
            print_usage(stdout);
            exit(0);
        case 'V':
            printf("driftpaper %s\n", VERSION);
            exit(0);
        default:
            print_usage(stderr);
            return 2;
        }
    }

    if (optind < argc) {
        static char absolute_image[PATH_MAX];
        opt->src.image = realpath(argv[optind], absolute_image) ? absolute_image : argv[optind];
    }
    return 0;
}

static void print_hex(const float *rgb) {
    printf(" #%02x%02x%02x", (int)(rgb[0] * 255 + 0.5f), (int)(rgb[1] * 255 + 0.5f), (int)(rgb[2] * 255 + 0.5f));
}

static int print_analysis(const struct config *cfg) {
    int w, h, channels;
    unsigned char *pixels = stbi_load(cfg->image, &w, &h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "driftpaper: cannot load image '%s': %s\n", cfg->image, stbi_failure_reason());
        return 1;
    }
    struct analysis an;
    analyze_image(pixels, w, h, cfg->ellipse, &an);
    stbi_image_free(pixels);

    printf("image:   %s (%dx%d)\n", cfg->image, w, h);
    if (an.found_subject)
        printf("subject: center = %.3f, %.3f  radius = %.3f of the image height\n", an.center_x, an.center_y, an.radius);
    else
        printf("subject: none found, particles fill the whole screen\n");
    printf("palette:");
    for (int i = 0; i < an.n_palette; i++) print_hex(an.palette[i]);
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    struct options opt;
    if (parse_options(argc, argv, &opt)) return 2;

    struct config cfg;
    if (settings_load(&opt.src, &cfg)) return 2;
    if (opt.mode == MODE_PRINT_CONFIG) {
        config_print(&cfg, stdout);
        return 0;
    }
    if (!cfg.image[0]) {
        fprintf(stderr, "driftpaper: no image given. Run 'driftpaper ~/Pictures/wall.png' or set image = ... in\n  %s\n",
                settings_config_path(&opt.src));
        return 2;
    }

    egl_select_renderer(cfg.renderer);
    switch (opt.mode) {
    case MODE_ANALYZE: return print_analysis(&cfg);
    case MODE_SNAPSHOT:
        return snapshot_write(&cfg, opt.snapshot_path, opt.snapshot_width, opt.snapshot_height, opt.snapshot_time,
                              opt.snapshot_music);
    default: return wallpaper_run(&opt.src, &cfg);
    }
}
