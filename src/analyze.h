#pragma once
#include "config.h"

struct analysis {
    int found_subject;
    float center_x, center_y;
    float radius;
    float palette[MAX_COLORS][3];
    int n_palette;
};

// Finds the image's main subject (centre and size, in fractions of the image height measured
// on an ellipse stretched by `ellipse`) and a vivid colour palette taken from it.
void analyze_image(const unsigned char *rgba, int w, int h, float ellipse, struct analysis *out);
