#pragma once
#include <EGL/egl.h>
#include "config.h"

struct egl {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
};

// Must run before the first EGL call: libglvnd picks its vendor library when it loads.
void egl_select_renderer(enum renderer renderer);
int egl_init(struct egl *egl, EGLNativeDisplayType native, EGLint surface_type);
void egl_finish(struct egl *egl);
