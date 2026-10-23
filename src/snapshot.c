#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <wayland-client.h>
#include "egl.h"
#include "runtime.h"
#include "stb_image_write.h"

// Returns the frame as top-down RGB rows, the layout PNG files use.
static unsigned char *render_frame_rgb(const struct renderer *r, const struct scene *s, int width, int height,
                                       struct viewport viewport, float canvas_aspect, float time,
                                       const struct music_levels *music) {
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    unsigned char *rgba = malloc((size_t)width * height * 4), *rgb = malloc((size_t)width * height * 3);
    if (rgba && rgb && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        render_draw(r, s, width, height, viewport, canvas_aspect, time, 1, music ? render_music_pulse(&s->cfg, music) : 0);
        if (music) render_music(r, s, width, height, viewport, canvas_aspect, time, music, s->cfg.music_badge, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        for (int y = 0; y < height; y++) {
            const unsigned char *from = rgba + (size_t)(height - 1 - y) * width * 4;
            unsigned char *to = rgb + (size_t)y * width * 3;
            for (int x = 0; x < width; x++, from += 4, to += 3) to[0] = from[0], to[1] = from[1], to[2] = from[2];
        }
    } else {
        free(rgb);
        rgb = NULL;
    }

    free(rgba);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    return rgb;
}

unsigned char *snapshot_render_rgb(const struct renderer *r, const struct scene *s, int width, int height,
                                   struct viewport viewport, float canvas_aspect) {
    return render_frame_rgb(r, s, width, height, viewport, canvas_aspect, SNAPSHOT_TIME, NULL);
}

int snapshot_save_png(const char *path, int width, int height, const unsigned char *rgb) {
    char temp[1100];
    snprintf(temp, sizeof temp, "%s.tmp", path);
    if (!stbi_write_png(temp, width, height, 3, rgb, width * 3) || rename(temp, path)) {
        fprintf(stderr, "driftpaper: cannot write '%s'\n", path);
        remove(temp);
        return -1;
    }
    return 0;
}

int snapshot_write(const struct config *cfg, const char *path, int width, int height, float time, int music) {
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
        struct music_levels sample;
        music_sample_levels(&sample);
        unsigned char *rgb = render_frame_rgb(&renderer, &scene, width, height, FULL_VIEWPORT, (float)width / height, time,
                                              music && cfg->music ? &sample : NULL);
        rc = rgb && !snapshot_save_png(path, width, height, rgb) ? 0 : 1;
        free(rgb);
        scene_destroy(&scene);
    }
    if (rc == 0) printf("wrote %s (%dx%d)\n", path, width, height);

    eglDestroySurface(egl.display, surface);
    egl_finish(&egl);
    if (display) wl_display_disconnect(display);
    return rc;
}
