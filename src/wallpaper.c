#define _GNU_SOURCE
#include "wallpaper.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "egl.h"
#include "render.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define MAX_OUTPUTS 16
#define RELOAD_CHECK_INTERVAL 1.0

struct wallpaper;

struct output {
    struct wallpaper *wp;
    uint32_t registry_name;
    struct wl_output *wl_output;
    char name[64];
    int announced;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_viewport *viewport;
    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;
    int width, height, buffer_width, buffer_height, configured, frame_pending;
};

struct wallpaper {
    const struct settings_source *src;
    struct config cfg;
    struct scene scene;
    int scene_ready;
    struct timespec config_mtime, image_mtime;

    struct wl_display *display;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;
    int warned_no_viewporter;
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

static void make_any_surface_current(struct wallpaper *wp) {
    for (int i = 0; i < wp->n_outputs; i++) {
        EGLSurface surface = wp->outputs[i].egl_surface;
        if (surface) {
            eglMakeCurrent(wp->egl.display, surface, surface, wp->egl.context);
            return;
        }
    }
}

static void prepare_scene(struct wallpaper *wp) {
    if (wp->scene_ready) return;
    if (render_init(&wp->scene) || render_load_image(&wp->scene, wp->cfg.image, wp->cfg.ellipse)) exit(1);
    render_apply_config(&wp->scene, &wp->cfg);
    wp->image_mtime = modification_time(wp->cfg.image);
    wp->scene_ready = 1;
}

// Renders at render_scale and lets the compositor stretch the buffer to the monitor on its GPU.
static void update_buffer_size(struct output *o) {
    struct wallpaper *wp = o->wp;
    float scale = config_render_scale(&wp->cfg);
    if (scale < 1 && !wp->viewporter) {
        if (!wp->warned_no_viewporter) fprintf(stderr, "driftpaper: compositor lacks wp_viewporter, ignoring render_scale\n");
        wp->warned_no_viewporter = 1;
        scale = 1;
    }
    o->buffer_width = (int)(o->width * scale + 0.5f);
    o->buffer_height = (int)(o->height * scale + 0.5f);

    if (scale < 1 && !o->viewport) o->viewport = wp_viewporter_get_viewport(wp->viewporter, o->surface);
    if (o->viewport) wp_viewport_set_destination(o->viewport, scale < 1 ? o->width : -1, scale < 1 ? o->height : -1);
    if (o->egl_window) wl_egl_window_resize(o->egl_window, o->buffer_width, o->buffer_height, 0, 0);
}

static void reload_if_changed(struct wallpaper *wp) {
    struct timespec config_mtime = modification_time(settings_config_path(wp->src));
    struct timespec image_mtime = modification_time(wp->cfg.image);
    int image_modified = !same_time(image_mtime, wp->image_mtime);
    if (same_time(config_mtime, wp->config_mtime) && !image_modified) return;
    wp->config_mtime = config_mtime;
    wp->image_mtime = image_mtime;

    struct config next;
    if (settings_load(wp->src, &next)) {
        fprintf(stderr, "driftpaper: config has errors, keeping the previous settings\n");
        return;
    }
    if (!wp->scene_ready) {
        wp->cfg = next;
        return;
    }

    make_any_surface_current(wp);
    int image_changed = strcmp(next.image, wp->cfg.image) || next.ellipse != wp->cfg.ellipse;
    if ((image_changed || image_modified) && render_load_image(&wp->scene, next.image, next.ellipse)) {
        fprintf(stderr, "driftpaper: keeping the previous image\n");
        snprintf(next.image, sizeof next.image, "%s", wp->cfg.image);
    }
    if (next.renderer != wp->cfg.renderer) fprintf(stderr, "driftpaper: restart driftpaper to switch the renderer\n");
    wp->cfg = next;
    wp->image_mtime = modification_time(wp->cfg.image);
    render_apply_config(&wp->scene, &wp->cfg);
    for (int i = 0; i < wp->n_outputs; i++)
        if (wp->outputs[i].configured) update_buffer_size(&wp->outputs[i]);
    fprintf(stderr, "driftpaper: reloaded settings\n");
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)time;
    wl_callback_destroy(callback);
    ((struct output *)data)->frame_pending = 0;
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void draw_output(struct output *o, float time) {
    struct wallpaper *wp = o->wp;
    eglMakeCurrent(wp->egl.display, o->egl_surface, o->egl_surface, wp->egl.context);
    render_draw(&wp->scene, o->buffer_width, o->buffer_height, time);
    o->frame_pending = 1;
    wl_callback_add_listener(wl_surface_frame(o->surface), &frame_listener, o);
    eglSwapBuffers(wp->egl.display, o->egl_surface);
}

static void destroy_surface(struct output *o) {
    struct egl *egl = &o->wp->egl;
    if (o->egl_surface) {
        eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl->context);
        eglDestroySurface(egl->display, o->egl_surface);
    }
    if (o->egl_window) wl_egl_window_destroy(o->egl_window);
    if (o->viewport) wp_viewport_destroy(o->viewport);
    if (o->layer_surface) zwlr_layer_surface_v1_destroy(o->layer_surface);
    if (o->surface) wl_surface_destroy(o->surface);
    o->egl_surface = EGL_NO_SURFACE;
    o->egl_window = NULL;
    o->layer_surface = NULL;
    o->viewport = NULL;
    o->surface = NULL;
    o->configured = o->frame_pending = 0;
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
        eglMakeCurrent(egl->display, o->egl_surface, o->egl_surface, egl->context);
        eglSwapInterval(egl->display, 0);
        prepare_scene(o->wp);
    }
    o->configured = 1;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface) {
    (void)layer_surface;
    destroy_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {.configure = layer_configure, .closed = layer_closed};

static void create_surface(struct output *o) {
    struct wallpaper *wp = o->wp;
    o->surface = wl_compositor_create_surface(wp->compositor);
    struct wl_region *no_input = wl_compositor_create_region(wp->compositor);
    wl_surface_set_input_region(o->surface, no_input);
    wl_region_destroy(no_input);

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

static void sync_surface(struct output *o) {
    if (!o->wl_output || !o->announced) return;
    int wanted = output_wanted(o->wp, o);
    if (wanted && !o->surface) create_surface(o);
    else if (!wanted && o->surface) destroy_surface(o);
}

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel, const char *make, const char *model,
                            int32_t transform) {}
static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height,
                        int32_t refresh) {}
static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {}
static void output_description(void *data, struct wl_output *wl_output, const char *description) {}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
    struct output *o = data;
    snprintf(o->name, sizeof o->name, "%s", name);
}

static void output_done(void *data, struct wl_output *wl_output) {
    struct output *o = data;
    o->announced = 1;
    sync_surface(o);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
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
    } else if (!strcmp(interface, wl_output_interface.name)) {
        struct output *o = free_output_slot(wp);
        if (!o) {
            fprintf(stderr, "driftpaper: more than %d monitors, ignoring one\n", MAX_OUTPUTS);
            return;
        }
        *o = (struct output){.wp = wp, .registry_name = name};
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

// Sandboxes such as Flatpak may hide WAYLAND_DISPLAY while still exposing the compositor's socket.
static struct wl_display *connect_display(void) {
    const char *name = getenv("WAYLAND_DISPLAY");
    if (name && *name) return wl_display_connect(NULL);
    for (int i = 0; i < 4; i++) {
        char socket_name[16];
        snprintf(socket_name, sizeof socket_name, "wayland-%d", i);
        struct wl_display *display = wl_display_connect(socket_name);
        if (display) return display;
    }
    return NULL;
}

static int connect_wayland(struct wallpaper *wp) {
    wp->display = connect_display();
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

static void draw_visible_outputs(struct wallpaper *wp, float time) {
    if (!wp->scene_ready) return;
    for (int i = 0; i < wp->n_outputs; i++) {
        struct output *o = &wp->outputs[i];
        // Covered monitors (e.g. a fullscreen game) never answer the frame callback, so they are skipped.
        if (o->configured && !o->frame_pending) draw_output(o, time);
    }
}

static void shutdown_wallpaper(struct wallpaper *wp) {
    for (int i = 0; i < wp->n_outputs; i++)
        if (wp->outputs[i].wl_output) destroy_surface(&wp->outputs[i]);
    egl_finish(&wp->egl);
    wl_display_disconnect(wp->display);
}

int wallpaper_run(const struct settings_source *src, const struct config *initial) {
    static struct wallpaper wp;
    wp.src = src;
    wp.cfg = *initial;
    wp.config_mtime = modification_time(settings_config_path(src));
    if (connect_wayland(&wp)) return 1;

    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    double start = monotonic_seconds(), next_frame = start, next_reload_check = start + RELOAD_CHECK_INTERVAL;
    while (!stop_requested) {
        if (wait_for_events(&wp, next_frame) < 0) {
            fprintf(stderr, "driftpaper: lost the Wayland connection\n");
            return 1;
        }

        double now = monotonic_seconds();
        if (now >= next_reload_check) {
            next_reload_check = now + RELOAD_CHECK_INTERVAL;
            reload_if_changed(&wp);
            for (int i = 0; i < wp.n_outputs; i++) sync_surface(&wp.outputs[i]);
        }
        if (now >= next_frame) {
            draw_visible_outputs(&wp, (float)(now - start));
            next_frame += 1.0 / config_fps(&wp.cfg);
            if (next_frame < now) next_frame = now + 1.0 / config_fps(&wp.cfg);
        }
    }
    shutdown_wallpaper(&wp);
    return 0;
}
