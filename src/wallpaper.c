#define _GNU_SOURCE
#include "wallpaper.h"
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "egl.h"
#include "fractional-scale-v1-client-protocol.h"
#include "lockscreen.h"
#include "music.h"
#include "render.h"
#include "runtime.h"
#include "snapshot.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define MAX_OUTPUTS 16
#define RELOAD_CHECK_INTERVAL 1.0
#define FADE_SECONDS 0.7
#define LOCK_SCREEN_DELAY 3.0
#define LOST_FRAME_TIMEOUT 2.0
#define RESTACK_DELAY 2.0
#define RESTACK_REPEAT 6.0
#define COVER_WATCH 6.0
#define STALLED_FRAME 0.4
#define COSMIC_BG_SCAN_INTERVAL 5.0
// Shaders get the animation time as a 32-bit float, which after a day or two is too coarse for
// smooth motion. The clock restarts (with a crossfade) well before that.
#define CLOCK_RESTART 14400.0

struct wallpaper;

struct output {
    struct wallpaper *wp;
    uint32_t registry_name;
    struct wl_output *wl_output;
    char name[64];
    int announced, x, y, integer_scale;
    double fractional_scale;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional;
    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;
    struct wl_callback *frame_callback;
    int width, height, buffer_width, buffer_height, configured, frame_pending;
    double frame_requested;
};

// All scenes built from one version of the settings: one per distinct picture, plus which scene
// each monitor shows, so a crossfade can find a monitor's old scene even after its picture changed.
// Looks are copied by value when a crossfade starts, so they refer to scenes by index, not pointer.
#define SCENE_UNRESOLVED 0
#define SCENE_FAILED -1

struct look {
    struct scene scenes[MAX_IMAGES];
    int n;
    int by_output[MAX_OUTPUTS];  // scene index + 1, or SCENE_UNRESOLVED / SCENE_FAILED
    char failed[MAX_IMAGES][PATH_LEN];
    int n_failed;
};

struct wallpaper {
    const struct settings_source *src;
    struct config cfg;
    struct renderer renderer;
    int renderer_ready, restacks_left;
    struct look current, previous;
    double fade_start, lock_screen_due, restack_due, next_cosmic_bg_scan, cover_watch_until;
    double clock_start, previous_clock_start;
    pid_t cosmic_bg;
    char music_source[sizeof ((struct config *)0)->music_source];
    struct timespec config_mtime;

    struct wl_display *display;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct output outputs[MAX_OUTPUTS];
    int n_outputs;
    struct egl egl;
};

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static struct timespec modification_time(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtim : (struct timespec){0};
}

static int same_time(struct timespec a, struct timespec b) {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

static int output_wanted(const struct wallpaper *wp, const struct output *o) {
    if (wp->cfg.n_outputs == 0) return 1;
    for (int i = 0; i < wp->cfg.n_outputs; i++)
        if (!strcmp(wp->cfg.outputs[i], o->name)) return 1;
    return 0;
}

static const char *image_for(const struct wallpaper *wp, const struct output *o) {
    return wp->cfg.span ? wp->cfg.image : config_image_for(&wp->cfg, o->name);
}

static int scene_with_image(struct wallpaper *wp, struct look *look, const char *image) {
    for (int i = 0; i < look->n; i++)
        if (!strcmp(look->scenes[i].image->path, image)) return i + 1;
    for (int i = 0; i < look->n_failed; i++)
        if (!strcmp(look->failed[i], image)) return SCENE_FAILED;
    if (look->n < MAX_IMAGES && !scene_create(&wp->renderer, &look->scenes[look->n], image, &wp->cfg))
        return ++look->n;
    if (look->n_failed < MAX_IMAGES) snprintf(look->failed[look->n_failed++], PATH_LEN, "%s", image);
    return SCENE_FAILED;
}

static struct scene *look_scene_for(struct look *look, const struct wallpaper *wp, const struct output *o) {
    int slot = look->by_output[o - wp->outputs];
    return slot > 0 ? &look->scenes[slot - 1] : NULL;
}

static struct scene *output_scene(struct wallpaper *wp, struct look *look, const struct output *o) {
    int index = (int)(o - wp->outputs);
    if (look->by_output[index] == SCENE_UNRESOLVED) look->by_output[index] = scene_with_image(wp, look, image_for(wp, o));
    return look_scene_for(look, wp, o);
}

static void look_clear(struct look *look) {
    for (int i = 0; i < look->n; i++) scene_destroy(&look->scenes[i]);
    memset(look, 0, sizeof *look);
}

static void make_any_surface_current(struct wallpaper *wp) {
    for (int i = 0; i < wp->n_outputs; i++) {
        EGLSurface surface = wp->outputs[i].egl_surface;
        if (surface) {
            eglMakeCurrent(wp->egl.display, surface, surface, wp->egl.context);
            return;
        }
    }
}

static double output_scale(const struct output *o) {
    return o->fractional_scale > 0 ? o->fractional_scale : o->integer_scale;
}

// Buffers are sized for the monitor's real pixel density times render_scale; the viewport then
// maps them onto the surface's logical size, so the compositor never has to stretch a small image.
static void update_buffer_size(struct output *o) {
    struct wallpaper *wp = o->wp;
    if (!o->width) return;
    double scale = output_scale(o);
    if (wp->viewporter) {
        scale *= config_render_scale(&wp->cfg);
        if (!o->viewport) o->viewport = wp_viewporter_get_viewport(wp->viewporter, o->surface);
        wp_viewport_set_destination(o->viewport, o->width, o->height);
    } else {
        scale = o->integer_scale;
        wl_surface_set_buffer_scale(o->surface, o->integer_scale);
    }
    o->buffer_width = (int)lround(o->width * scale);
    o->buffer_height = (int)lround(o->height * scale);
    if (o->egl_window) wl_egl_window_resize(o->egl_window, o->buffer_width, o->buffer_height, 0, 0);
}

static int span_bounds(const struct wallpaper *wp, int *x0, int *y0, int *x1, int *y1) {
    int found = 0;
    for (int i = 0; i < wp->n_outputs; i++) {
        const struct output *o = &wp->outputs[i];
        if (!o->configured) continue;
        if (!found || o->x < *x0) *x0 = o->x;
        if (!found || o->y < *y0) *y0 = o->y;
        if (!found || o->x + o->width > *x1) *x1 = o->x + o->width;
        if (!found || o->y + o->height > *y1) *y1 = o->y + o->height;
        found = 1;
    }
    return found;
}

static struct viewport output_viewport(const struct output *o, float *canvas_aspect) {
    int x0, y0, x1, y1;
    *canvas_aspect = (float)o->width / o->height;
    if (!o->wp->cfg.span || !span_bounds(o->wp, &x0, &y0, &x1, &y1)) return FULL_VIEWPORT;
    float w = x1 - x0, h = y1 - y0;
    *canvas_aspect = w / h;
    return (struct viewport){(o->x - x0) / w, (o->y - y0) / h, o->width / w, o->height / h};
}

// Compressing a PNG per monitor takes about a second, so it happens on a thread of its own and
// the animation keeps running. Only one export runs at a time.
struct lock_screen_job {
    int count;
    struct {
        char output[64];
        int width, height;
        unsigned char *rgb;
    } images[MAX_OUTPUTS];
};

static atomic_int lock_screen_writing, lock_screen_written;

static void *write_lock_screen(void *data) {
    struct lock_screen_job *job = data;
    const char *names[MAX_OUTPUTS];
    int count = 0;
    lockscreen_prepare();
    for (int i = 0; i < job->count; i++) {
        char path[1024];
        lockscreen_image_path(job->images[i].output, path, sizeof path);
        if (!snapshot_save_png(path, job->images[i].width, job->images[i].height, job->images[i].rgb))
            names[count++] = job->images[i].output;
    }
    if (lockscreen_apply(names, count)) fprintf(stderr, "driftpaper: could not update the COSMIC background config\n");
    for (int i = 0; i < job->count; i++) free(job->images[i].rgb);
    free(job);
    atomic_store(&lock_screen_written, 1);
    atomic_store(&lock_screen_writing, 0);
    return NULL;
}

static void export_lock_screen(struct wallpaper *wp) {
    struct lock_screen_job *job = calloc(1, sizeof *job);
    if (!job) return;
    for (int i = 0; i < wp->n_outputs; i++) {
        struct output *o = &wp->outputs[i];
        struct scene *scene = o->configured ? output_scene(wp, &wp->current, o) : NULL;
        if (!scene) continue;
        float aspect;
        struct viewport viewport = output_viewport(o, &aspect);
        int width = (int)lround(o->width * output_scale(o)), height = (int)lround(o->height * output_scale(o));
        make_any_surface_current(wp);
        unsigned char *rgb = snapshot_render_rgb(&wp->renderer, scene, width, height, viewport, aspect);
        if (!rgb) continue;
        snprintf(job->images[job->count].output, sizeof job->images[0].output, "%s", o->name);
        job->images[job->count].width = width;
        job->images[job->count].height = height;
        job->images[job->count++].rgb = rgb;
    }
    if (!job->count) {
        free(job);
        return;
    }

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    atomic_store(&lock_screen_writing, 1);
    if (pthread_create(&thread, &attr, write_lock_screen, job)) write_lock_screen(job);
    pthread_attr_destroy(&attr);
}

static void schedule_lock_screen(struct wallpaper *wp) {
    wp->lock_screen_due = wp->cfg.lock_screen_sync && lockscreen_supported() ? monotonic_seconds() + LOCK_SCREEN_DELAY : 0;
}

static void start_crossfade(struct wallpaper *wp) {
    make_any_surface_current(wp);
    look_clear(&wp->previous);
    wp->previous = wp->current;
    wp->previous_clock_start = wp->clock_start;
    memset(&wp->current, 0, sizeof wp->current);
    wp->fade_start = monotonic_seconds();
}

static void restart_clock_if_due(struct wallpaper *wp, double now) {
    if (now - wp->clock_start < CLOCK_RESTART || wp->previous.n || !wp->renderer_ready) return;
    start_crossfade(wp);
    wp->clock_start = now;
}

static void sync_music(struct wallpaper *wp) {
    int running = music_running();
    if (!wp->cfg.music) {
        if (running) music_stop();
        return;
    }
    if (running && !strcmp(wp->music_source, wp->cfg.music_source)) return;
    snprintf(wp->music_source, sizeof wp->music_source, "%s", wp->cfg.music_source);
    if (music_start(wp->music_source)) fprintf(stderr, "driftpaper: music sync is unavailable\n");
}

static int only_music_changed(const struct config *a, const struct config *b) {
    return !memcmp(a, b, offsetof(struct config, music));
}

// The visualiser is drawn from each scene's copy of the settings, so music settings can be changed
// in place, without rebuilding the scenes.
static void update_music_settings(struct look *look, const struct config *cfg) {
    size_t from = offsetof(struct config, music);
    for (int i = 0; i < look->n; i++)
        memcpy((char *)&look->scenes[i].cfg + from, (const char *)cfg + from, sizeof *cfg - from);
}

static void reload_if_changed(struct wallpaper *wp) {
    struct timespec config_mtime = modification_time(settings_config_path(wp->src));
    int images_changed = 0;
    // Only the pictures on screen now: the one fading out is still the old version of an edited file.
    for (int i = 0; i < wp->current.n; i++) {
        const struct image *img = wp->current.scenes[i].image;
        if (!same_time(modification_time(img->path), img->mtime)) images_changed = 1;
    }
    if (same_time(config_mtime, wp->config_mtime) && !images_changed) return;
    wp->config_mtime = config_mtime;

    struct config next;
    if (settings_load(wp->src, &next)) {
        fprintf(stderr, "driftpaper: config has errors, keeping the previous settings\n");
        return;
    }
    // Editors that rewrite the file in place can be caught halfway, with the picture not written yet.
    if (!next.image[0]) {
        fprintf(stderr, "driftpaper: config has no image, keeping the previous settings\n");
        return;
    }
    if (!next.enabled) {
        fprintf(stderr, "driftpaper: wallpaper disabled in the settings, exiting\n");
        stop_requested = 1;
        return;
    }
    if (!images_changed && !memcmp(&next, &wp->cfg, sizeof next)) return;
    if (next.renderer != wp->cfg.renderer) fprintf(stderr, "driftpaper: restart driftpaper to switch the renderer\n");
    int music_only = !images_changed && only_music_changed(&next, &wp->cfg);
    wp->cfg = next;
    sync_music(wp);
    // The lock screen is exported without the visualiser, so music settings leave it as it is.
    if (music_only) {
        update_music_settings(&wp->current, &wp->cfg);
        update_music_settings(&wp->previous, &wp->cfg);
        return;
    }
    if (wp->renderer_ready) start_crossfade(wp);
    for (int i = 0; i < wp->n_outputs; i++)
        if (wp->outputs[i].configured) update_buffer_size(&wp->outputs[i]);
    schedule_lock_screen(wp);
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    struct output *o = data;
    wl_callback_destroy(callback);
    o->frame_callback = NULL;
    o->frame_pending = 0;
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

// `music` is NULL while music sync is off.
static void draw_output(struct output *o, double now, float fade, const struct music_levels *music, int badge) {
    struct wallpaper *wp = o->wp;
    eglMakeCurrent(wp->egl.display, o->egl_surface, o->egl_surface, wp->egl.context);

    struct scene *scene = output_scene(wp, &wp->current, o);
    struct scene *fading = fade < 1 ? look_scene_for(&wp->previous, wp, o) : NULL;
    if (!scene && !fading) return;
    float aspect, time = (float)(now - wp->clock_start), fading_time = (float)(now - wp->previous_clock_start);
    float pulse = music ? render_music_pulse(&wp->cfg, music) : 0;
    struct viewport viewport = output_viewport(o, &aspect);
    if (scene) render_draw(&wp->renderer, scene, o->buffer_width, o->buffer_height, viewport, aspect, time, 1, pulse);
    if (fading)
        render_draw(&wp->renderer, fading, o->buffer_width, o->buffer_height, viewport, aspect, fading_time,
                    scene ? 1 - fade : 1, pulse);
    if (music)
        render_music(&wp->renderer, scene ? scene : fading, o->buffer_width, o->buffer_height, viewport, aspect,
                     scene ? time : fading_time, music, badge, 1);

    o->frame_pending = 1;
    o->frame_requested = now;
    o->frame_callback = wl_surface_frame(o->surface);
    wl_callback_add_listener(o->frame_callback, &frame_listener, o);
    eglSwapBuffers(wp->egl.display, o->egl_surface);
}

static void destroy_surface(struct output *o) {
    struct egl *egl = &o->wp->egl;
    if (o->egl_surface) {
        eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl->context);
        eglDestroySurface(egl->display, o->egl_surface);
    }
    if (o->egl_window) wl_egl_window_destroy(o->egl_window);
    if (o->frame_callback) wl_callback_destroy(o->frame_callback);
    if (o->fractional) wp_fractional_scale_v1_destroy(o->fractional);
    if (o->viewport) wp_viewport_destroy(o->viewport);
    if (o->layer_surface) zwlr_layer_surface_v1_destroy(o->layer_surface);
    if (o->surface) wl_surface_destroy(o->surface);
    o->egl_surface = EGL_NO_SURFACE;
    o->egl_window = NULL;
    o->frame_callback = NULL;
    o->fractional = NULL;
    o->viewport = NULL;
    o->layer_surface = NULL;
    o->surface = NULL;
    o->configured = o->frame_pending = 0;
}

static void prepare_renderer(struct wallpaper *wp) {
    if (wp->renderer_ready) return;
    if (renderer_init(&wp->renderer)) exit(1);
    wp->renderer_ready = 1;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
                            uint32_t width, uint32_t height) {
    struct output *o = data;
    struct egl *egl = &o->wp->egl;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    if (!width || !height) return;
    o->width = width;
    o->height = height;
    update_buffer_size(o);

    if (!o->egl_window) {
        o->egl_window = wl_egl_window_create(o->surface, o->buffer_width, o->buffer_height);
        o->egl_surface = eglCreateWindowSurface(egl->display, egl->config, (EGLNativeWindowType)o->egl_window, NULL);
        if (o->egl_surface == EGL_NO_SURFACE) {
            fprintf(stderr, "driftpaper: cannot draw on monitor %s (EGL error 0x%x)\n", o->name, eglGetError());
            wl_egl_window_destroy(o->egl_window);
            o->egl_window = NULL;
            return;
        }
        eglMakeCurrent(egl->display, o->egl_surface, o->egl_surface, egl->context);
        eglSwapInterval(egl->display, 0);
        prepare_renderer(o->wp);
    }
    o->configured = 1;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface) {
    destroy_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {.configure = layer_configure, .closed = layer_closed};

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *fractional, uint32_t scale) {
    struct output *o = data;
    o->fractional_scale = scale / 120.0;
    update_buffer_size(o);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {.preferred_scale = preferred_scale};

static void create_surface(struct output *o) {
    struct wallpaper *wp = o->wp;
    o->surface = wl_compositor_create_surface(wp->compositor);
    struct wl_region *no_input = wl_compositor_create_region(wp->compositor);
    wl_surface_set_input_region(o->surface, no_input);
    wl_region_destroy(no_input);
    if (wp->fractional_manager) {
        o->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(wp->fractional_manager, o->surface);
        wp_fractional_scale_v1_add_listener(o->fractional, &fractional_listener, o);
    }

    o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(wp->layer_shell, o->surface, o->wl_output,
                                                             ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "driftpaper");
    zwlr_layer_surface_v1_set_anchor(o->layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer_surface, 0);
    zwlr_layer_surface_v1_add_listener(o->layer_surface, &layer_listener, o);
    wl_surface_commit(o->surface);
}

static void schedule_restack(struct wallpaper *wp, double now);

static void sync_surface(struct output *o) {
    if (!o->wl_output || !o->announced) return;
    int wanted = output_wanted(o->wp, o);
    if (wanted && !o->surface) create_surface(o);
    else if (!wanted && o->surface) destroy_surface(o);
}

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel, const char *make, const char *model,
                            int32_t transform) {
    struct output *o = data;
    o->x = x;
    o->y = y;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height,
                        int32_t refresh) {}
static void output_description(void *data, struct wl_output *wl_output, const char *description) {}

static void output_scale_changed(void *data, struct wl_output *wl_output, int32_t factor) {
    struct output *o = data;
    o->integer_scale = factor;
    if (o->surface) update_buffer_size(o);
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
    struct output *o = data;
    snprintf(o->name, sizeof o->name, "%s", name);
}

static void output_done(void *data, struct wl_output *wl_output) {
    struct output *o = data;
    o->announced = 1;
    sync_surface(o);
    // A monitor that wakes up, is plugged in or changes mode gets a new cosmic-bg surface, which
    // can end up on top of ours.
    if (o->wp->clock_start) {
        double now = monotonic_seconds();
        schedule_restack(o->wp, now);
        o->wp->cover_watch_until = now + COVER_WATCH;
    }
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale_changed,
    .name = output_name,
    .description = output_description,
};

static struct output *free_output_slot(struct wallpaper *wp) {
    for (int i = 0; i < wp->n_outputs; i++)
        if (!wp->outputs[i].wl_output) return &wp->outputs[i];
    return wp->n_outputs < MAX_OUTPUTS ? &wp->outputs[wp->n_outputs++] : NULL;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                            uint32_t version) {
    struct wallpaper *wp = data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        wp->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        wp->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        wp->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        wp->fractional_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        struct output *o = free_output_slot(wp);
        if (!o) {
            fprintf(stderr, "driftpaper: more than %d monitors, ignoring one\n", MAX_OUTPUTS);
            return;
        }
        *o = (struct output){.wp = wp, .registry_name = name, .integer_scale = 1};
        o->wl_output = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
        wl_output_add_listener(o->wl_output, &output_listener, o);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    struct wallpaper *wp = data;
    for (int i = 0; i < wp->n_outputs; i++) {
        struct output *o = &wp->outputs[i];
        if (!o->wl_output || o->registry_name != name) continue;
        destroy_surface(o);
        wp->current.by_output[i] = wp->previous.by_output[i] = SCENE_UNRESOLVED;
        wl_output_destroy(o->wl_output);
        o->wl_output = NULL;
        o->announced = 0;
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int connect_wayland(struct wallpaper *wp) {
    runtime_find_wayland_display();
    wp->display = wl_display_connect(NULL);
    if (!wp->display) {
        fprintf(stderr, "driftpaper: cannot connect to a Wayland display\n");
        return -1;
    }
    if (egl_init(&wp->egl, (EGLNativeDisplayType)wp->display, EGL_WINDOW_BIT)) return -1;

    wl_registry_add_listener(wl_display_get_registry(wp->display), &registry_listener, wp);
    wl_display_roundtrip(wp->display);
    if (!wp->compositor || !wp->layer_shell) {
        fprintf(stderr, "driftpaper: this compositor does not support wlr-layer-shell, which wallpapers need\n");
        return -1;
    }
    wl_display_roundtrip(wp->display);
    for (int i = 0; i < wp->n_outputs; i++) sync_surface(&wp->outputs[i]);
    return 0;
}

static int wait_for_events(struct wallpaper *wp, double deadline) {
    while (wl_display_prepare_read(wp->display) != 0) wl_display_dispatch_pending(wp->display);
    wl_display_flush(wp->display);

    double remaining = deadline - monotonic_seconds();
    if (remaining < 0) remaining = 0;
    struct timespec timeout = {(time_t)remaining, (long)((remaining - (time_t)remaining) * 1e9)};
    struct pollfd pfd = {.fd = wl_display_get_fd(wp->display), .events = POLLIN};
    if (ppoll(&pfd, 1, &timeout, NULL) > 0) wl_display_read_events(wp->display);
    else wl_display_cancel_read(wp->display);
    return wl_display_dispatch_pending(wp->display);
}

// The badge goes in the bottom-right corner of the whole desktop, not on every monitor.
static int badge_output(const struct wallpaper *wp) {
    int best = -1;
    for (int i = 0; i < wp->n_outputs; i++) {
        const struct output *o = &wp->outputs[i], *b = best >= 0 ? &wp->outputs[best] : NULL;
        if (!o->configured) continue;
        int right = o->x + o->width, bottom = o->y + o->height;
        if (!b || right > b->x + b->width || (right == b->x + b->width && bottom > b->y + b->height)) best = i;
    }
    return best;
}

static void draw_visible_outputs(struct wallpaper *wp, double now) {
    if (!wp->renderer_ready) return;
    struct music_levels levels, *music = NULL;
    if (wp->cfg.music) {
        music_read(&levels);
        music = &levels;
    }
    int badge = wp->cfg.music && wp->cfg.music_badge ? badge_output(wp) : -1;
    float fade = wp->previous.n ? (float)((now - wp->fade_start) / FADE_SECONDS) : 1;
    if (fade >= 1 && wp->previous.n) {
        make_any_surface_current(wp);
        look_clear(&wp->previous);
    }
    for (int i = 0; i < wp->n_outputs; i++) {
        struct output *o = &wp->outputs[i];
        // Covered monitors (e.g. a fullscreen game) never answer the frame callback, so they are skipped.
        // COSMIC can also drop a callback while it sets up outputs at login; without this retry
        // that monitor would stay frozen on its first frame.
        if (o->frame_pending && now - o->frame_requested > LOST_FRAME_TIMEOUT) o->frame_pending = 0;
        if (o->configured && o->egl_surface && !o->frame_pending) draw_output(o, now, fade, music, i == badge);
    }
}

// Surfaces in the same layer stack in the order they appear, so when cosmic-bg starts (at login it
// starts at the same moment as us) or reloads its config (after every lock screen export) its static
// picture can end up covering the animation. Making our surfaces again, once cosmic-bg has had time
// to show its own, puts them back on top.
static void schedule_restack(struct wallpaper *wp, double now) {
    wp->restack_due = now + RESTACK_DELAY;
    wp->restacks_left = 2;
}

static void restack(struct wallpaper *wp) {
    for (int i = 0; i < wp->n_outputs; i++) {
        if (!wp->outputs[i].surface) continue;
        destroy_surface(&wp->outputs[i]);
        sync_surface(&wp->outputs[i]);
    }
}

static void restack_if_due(struct wallpaper *wp, double now) {
    if (!wp->restack_due || now < wp->restack_due) return;
    wp->restack_due = --wp->restacks_left > 0 ? now + RESTACK_REPEAT : 0;
    restack(wp);
}

// After a lock screen export cosmic-bg covers us within about a second, and from then on our
// frames are no longer shown. Restacking as soon as one stalls keeps that freeze short; the
// scheduled restacks remain as a fallback.
static void restack_if_covered(struct wallpaper *wp, double now) {
    if (now > wp->cover_watch_until) return;
    for (int i = 0; i < wp->n_outputs; i++) {
        const struct output *o = &wp->outputs[i];
        if (o->frame_pending && now - o->frame_requested > STALLED_FRAME) {
            wp->cover_watch_until = 0;
            restack(wp);
            return;
        }
    }
}

static void watch_cosmic_bg(struct wallpaper *wp, double now) {
    if (!lockscreen_supported()) return;
    // Without a known cosmic-bg each check scans /proc, so do that less often.
    if (!wp->cosmic_bg && now < wp->next_cosmic_bg_scan) return;
    wp->next_cosmic_bg_scan = now + COSMIC_BG_SCAN_INTERVAL;
    pid_t pid = runtime_cosmic_bg_pid(wp->cosmic_bg);
    if (pid && pid != wp->cosmic_bg) schedule_restack(wp, now);
    wp->cosmic_bg = pid;
}

static int lock_screen_outdated(const struct wallpaper *wp) {
    if (!wp->cfg.lock_screen_sync || !lockscreen_supported()) return 0;
    for (int i = 0; i < wp->n_outputs; i++) {
        char path[1024];
        lockscreen_image_path(wp->outputs[i].name, path, sizeof path);
        struct timespec png = modification_time(path);
        if (wp->outputs[i].announced && png.tv_sec < wp->config_mtime.tv_sec) return 1;
    }
    return 0;
}

static void shutdown_wallpaper(struct wallpaper *wp) {
    music_stop();
    make_any_surface_current(wp);
    look_clear(&wp->previous);
    look_clear(&wp->current);
    for (int i = 0; i < wp->n_outputs; i++)
        if (wp->outputs[i].wl_output) destroy_surface(&wp->outputs[i]);
    egl_finish(&wp->egl);
    wl_display_disconnect(wp->display);
}

int wallpaper_run(const struct settings_source *src, const struct config *initial) {
    if (!initial->enabled) {
        fprintf(stderr, "driftpaper: wallpaper is disabled (enabled = false), not starting\n");
        return 0;
    }
    int lock = runtime_acquire_lock();
    if (lock == -1) {
        fprintf(stderr, "driftpaper: replacing the running wallpaper\n");
        lock = runtime_take_over();
        if (lock == -1) {
            fprintf(stderr, "driftpaper: the running wallpaper did not stop, giving up\n");
            return 1;
        }
    }
    struct timespec started;
    clock_gettime(CLOCK_REALTIME, &started);

    static struct wallpaper wp;
    wp.src = src;
    wp.cfg = *initial;
    wp.config_mtime = modification_time(settings_config_path(src));
    if (connect_wayland(&wp)) return 1;
    if (lock_screen_outdated(&wp)) schedule_lock_screen(&wp);

    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    double start = monotonic_seconds(), next_frame = start, next_reload_check = start + RELOAD_CHECK_INTERVAL;
    wp.clock_start = wp.previous_clock_start = start;
    sync_music(&wp);
    // cosmic-bg may not be visible from here (Flatpak), so always check the stacking after starting.
    if (lockscreen_supported()) schedule_restack(&wp, start);
    while (!stop_requested) {
        if (wait_for_events(&wp, next_frame) < 0) {
            fprintf(stderr, "driftpaper: lost the Wayland connection\n");
            return 1;
        }

        double now = monotonic_seconds();
        if (now >= next_reload_check) {
            next_reload_check = now + RELOAD_CHECK_INTERVAL;
            if (runtime_takeover_requested(started)) {
                fprintf(stderr, "driftpaper: another wallpaper took over, exiting\n");
                break;
            }
            reload_if_changed(&wp);
            for (int i = 0; i < wp.n_outputs; i++) sync_surface(&wp.outputs[i]);
            watch_cosmic_bg(&wp, now);
            restack_if_due(&wp, now);
            restart_clock_if_due(&wp, now);
            if (wp.lock_screen_due && now >= wp.lock_screen_due && wp.renderer_ready && !atomic_load(&lock_screen_writing)) {
                wp.lock_screen_due = 0;
                export_lock_screen(&wp);
            }
            if (atomic_exchange(&lock_screen_written, 0)) {
                wp.cover_watch_until = now + COVER_WATCH;
                schedule_restack(&wp, now);
            }
        }
        if (now >= next_frame) {
            restack_if_covered(&wp, now);
            draw_visible_outputs(&wp, now);
            next_frame += 1.0 / config_fps(&wp.cfg);
            if (next_frame < now) next_frame = now + 1.0 / config_fps(&wp.cfg);
        }
    }
    shutdown_wallpaper(&wp);
    return 0;
}
