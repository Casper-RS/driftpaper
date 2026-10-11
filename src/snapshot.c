#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <wayland-client.h>
#include "egl.h"
#include "runtime.h"
#include "stb_image_write.h"

static int render_frame_png(const struct renderer *r, const struct scene *s, int width, int height,
                            struct viewport viewport, float canvas_aspect, float time, const char *path) {
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    int written = 0;
    unsigned char *pixels = malloc((size_t)width * height * 4);
    if (pixels && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        render_draw(r, s, width, height, viewport, canvas_aspect, time, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        for (size_t i = 0; i < (size_t)width * height; i++) pixels[4 * i + 3] = 255;
        stbi_flip_vertically_on_write(1);
        written = stbi_write_png(path, width, height, 4, pixels, width * 4);
    }

    free(pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    if (!written) fprintf(stderr, "driftpaper: cannot write '%s'\n", path);
    return written ? 0 : -1;
}

int snapshot_render_png(const struct renderer *r, const struct scene *s, int width, int height,
                        struct viewport viewport, float canvas_aspect, const char *path) {
    return render_frame_png(r, s, width, height, viewport, canvas_aspect, SNAPSHOT_TIME, path);
}

int snapshot_write(const struct config *cfg, const char *path, int width, int height, float time) {
    runtime_find_wayland_display();
    struct wl_display *display = wl_display_connect(NULL);
    EGLNativeDisplayType native = display ? (EGLNativeDisplayType)display : EGL_DEFAULT_DISPLAY;
    struct egl egl;
    if (egl_init(&egl, native, EGL_PBUFFER_BIT)) return 1;

    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(egl.display, egl.config, size);
    eglMakeCurrent(egl.display, surface, surface, egl.context);

    struct renderer renderer;
    struct scene scene;
    int rc = 1;
    if (!renderer_init(&renderer) && !scene_create(&renderer, &scene, cfg->image, cfg)) {
        rc = render_frame_png(&renderer, &scene, width, height, FULL_VIEWPORT, (float)width / height, time, path) ? 1 : 0;
        scene_destroy(&scene);
    }
    if (rc == 0) printf("wrote %s (%dx%d)\n", path, width, height);

    eglDestroySurface(egl.display, surface);
    egl_finish(&egl);
    if (display) wl_display_disconnect(display);
    return rc;
}
