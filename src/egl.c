#define _GNU_SOURCE
#include "egl.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int find_vendor_file(const char *keyword, char *out, size_t size) {
    char dirs[4096];
    const char *extra = getenv("__EGL_VENDOR_LIBRARY_DIRS");
    snprintf(dirs, sizeof dirs, "%s%s%s", extra ? extra : "", extra ? ":" : "",
             "/etc/glvnd/egl_vendor.d:/usr/share/glvnd/egl_vendor.d:/usr/lib/x86_64-linux-gnu/GL/glvnd/egl_vendor.d");

    for (char *save, *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        DIR *d = opendir(dir);
        if (!d) continue;
        for (struct dirent *entry; (entry = readdir(d));) {
            if (strstr(entry->d_name, keyword) && strstr(entry->d_name, ".json")) {
                snprintf(out, size, "%s/%s", dir, entry->d_name);
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    return 0;
}

void egl_select_renderer(enum renderer_kind renderer) {
    char vendor[1024];
    if (renderer == RENDERER_GPU) {
        if (find_vendor_file("nvidia", vendor, sizeof vendor)) setenv("__EGL_VENDOR_LIBRARY_FILENAMES", vendor, 1);
        setenv("DRI_PRIME", "1", 0);
    } else if (renderer == RENDERER_CPU) {
        if (find_vendor_file("mesa", vendor, sizeof vendor)) setenv("__EGL_VENDOR_LIBRARY_FILENAMES", vendor, 1);
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    }
}

int egl_init(struct egl *egl, EGLNativeDisplayType native, EGLint surface_type) {
    egl->display = eglGetDisplay(native);
    if (egl->display == EGL_NO_DISPLAY || !eglInitialize(egl->display, NULL, NULL)) {
        fprintf(stderr, "driftpaper: cannot initialise EGL\n");
        return -1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, surface_type,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLint matches;
    if (!eglChooseConfig(egl->display, config_attributes, &egl->config, 1, &matches) || matches < 1) {
        fprintf(stderr, "driftpaper: no suitable EGL config\n");
        return -1;
    }

    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl->context = eglCreateContext(egl->display, egl->config, EGL_NO_CONTEXT, context_attributes);
    if (egl->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "driftpaper: cannot create an OpenGL ES 2 context\n");
        return -1;
    }
    return 0;
}

void egl_finish(struct egl *egl) {
    eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl->display, egl->context);
    eglTerminate(egl->display);
}
