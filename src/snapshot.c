#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <wayland-client.h>
#include "egl.h"
#include "render.h"
#include "stb_image.h"
#include "stb_image_write.h"

static int render_offscreen(const struct config *cfg, int width, int height, float time, unsigned char *pixels) {
    struct wl_display *display = wl_display_connect(NULL);
    EGLNativeDisplayType native = display ? (EGLNativeDisplayType)display : EGL_DEFAULT_DISPLAY;
    struct egl egl;
    if (egl_init(&egl, native, EGL_PBUFFER_BIT)) return -1;

    const EGLint size[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(egl.display, egl.config, size);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "driftpaper: cannot create a %dx%d offscreen surface\n", width, height);
        return -1;
    }
    eglMakeCurrent(egl.display, surface, surface, egl.context);

    struct scene scene;
    int rc = render_init(&scene) || render_load_image(&scene, cfg->image, cfg->ellipse) ? -1 : 0;
    if (rc == 0) {
        render_apply_config(&scene, cfg);
        render_draw(&scene, width, height, time);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    eglDestroySurface(egl.display, surface);
    egl_finish(&egl);
    if (display) wl_display_disconnect(display);
    return rc;
}

int snapshot_write(const struct config *cfg, const char *path, int width, int height, float time) {
    unsigned char *pixels = malloc((size_t)width * height * 4);
    if (render_offscreen(cfg, width, height, time, pixels)) {
        free(pixels);
        return 1;
    }
    for (size_t i = 0; i < (size_t)width * height; i++) pixels[4 * i + 3] = 255;

    stbi_flip_vertically_on_write(1);
    int written = stbi_write_png(path, width, height, 4, pixels, width * 4);
    free(pixels);
    if (!written) {
        fprintf(stderr, "driftpaper: cannot write '%s'\n", path);
        return 1;
    }
    printf("wrote %s (%dx%d)\n", path, width, height);
    return 0;
}
