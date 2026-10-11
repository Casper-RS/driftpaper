#include "analyze.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ANALYSIS_WIDTH 320
#define CLUSTER_COUNT 6
#define KMEANS_ITERATIONS 12
#define MAX_PALETTE 4
#define SUBJECT_QUANTILE 0.75f
#define MAX_SUBJECT_RADIUS 0.38f

struct image {
    float *rgb;
    int width, height;
};

static int compare_floats(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float quantile(float *values, int n, float q) {
    qsort(values, n, sizeof(float), compare_floats);
    return values[(int)(n * q)];
}

static float color_distance_sq(const float *a, const float *b) {
    float dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}

static float chroma(const float *c) {
    return fmaxf(c[0], fmaxf(c[1], c[2])) - fminf(c[0], fminf(c[1], c[2]));
}

static void rgb_to_hsv(const float *c, float *h, float *s, float *v) {
    float max = fmaxf(c[0], fmaxf(c[1], c[2])), delta = chroma(c);
    *v = max;
    *s = max > 0 ? delta / max : 0;
    if (delta <= 0) *h = 0;
    else if (max == c[0]) *h = 60 * fmodf((c[1] - c[2]) / delta + 6, 6);
    else if (max == c[1]) *h = 60 * ((c[2] - c[0]) / delta + 2);
    else *h = 60 * ((c[0] - c[1]) / delta + 4);
}

static void hsv_to_rgb(float h, float s, float v, float *out) {
    float sector = h / 60, x = v * (1 - s * fabsf(fmodf(sector, 2) - 1)), low = v * (1 - s);
    const float table[6][3] = {{v, x, low}, {x, v, low}, {low, v, x}, {low, x, v}, {x, low, v}, {v, low, x}};
    memcpy(out, table[(int)sector % 6], 3 * sizeof(float));
}

static struct image downscale(const unsigned char *rgba, int w, int h) {
    struct image img = {.width = w < ANALYSIS_WIDTH ? w : ANALYSIS_WIDTH};
    img.height = (int)((float)h * img.width / w + 0.5f);
    if (img.height < 1) img.height = 1;
    img.rgb = malloc(sizeof(float) * 3 * img.width * img.height);

    for (int y = 0; y < img.height; y++) {
        int y0 = y * h / img.height, y1 = (y + 1) * h / img.height;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < img.width; x++) {
            int x0 = x * w / img.width, x1 = (x + 1) * w / img.width;
            if (x1 <= x0) x1 = x0 + 1;
            double sum[3] = {0};
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++)
                    for (int ch = 0; ch < 3; ch++) sum[ch] += rgba[4 * (yy * w + xx) + ch];
            int count = (x1 - x0) * (y1 - y0);
            for (int ch = 0; ch < 3; ch++) img.rgb[3 * (y * img.width + x) + ch] = (float)(sum[ch] / (count * 255.0));
        }
    }
    return img;
}

static void border_color(const struct image *img, float out[3]) {
    int bw = img->width / 25 + 1, bh = img->height / 25 + 1, count = 0;
    double sum[3] = {0};
    for (int y = 0; y < img->height; y++)
        for (int x = 0; x < img->width; x++) {
            if (x >= bw && x < img->width - bw && y >= bh && y < img->height - bh) continue;
            for (int ch = 0; ch < 3; ch++) sum[ch] += img->rgb[3 * (y * img->width + x) + ch];
            count++;
        }
    for (int ch = 0; ch < 3; ch++) out[ch] = (float)(sum[ch] / count);
}

static int kmeans(const float *colors, int n, float means[CLUSTER_COUNT][3], int sizes[CLUSTER_COUNT]) {
    int k = n < CLUSTER_COUNT ? n : CLUSTER_COUNT;
    if (k == 0) return 0;

    int start = 0;
    for (int i = 1; i < n; i++)
        if (chroma(colors + 3 * i) > chroma(colors + 3 * start)) start = i;
    memcpy(means[0], colors + 3 * start, sizeof means[0]);

    // Farthest-point seeding (on every third colour, for speed) spreads the initial means across the gamut.
    for (int j = 1; j < k; j++) {
        float farthest = -1;
        int pick = 0;
        for (int i = 0; i < n; i += 3) {
            float nearest = INFINITY;
            for (int m = 0; m < j; m++) nearest = fminf(nearest, color_distance_sq(colors + 3 * i, means[m]));
            if (nearest > farthest) { farthest = nearest; pick = i; }
        }
        memcpy(means[j], colors + 3 * pick, sizeof means[j]);
    }

    for (int iter = 0; iter < KMEANS_ITERATIONS; iter++) {
        double sums[CLUSTER_COUNT][3] = {{0}};
        memset(sizes, 0, CLUSTER_COUNT * sizeof(int));
        for (int i = 0; i < n; i++) {
            int best = 0;
            float best_dist = INFINITY;
            for (int m = 0; m < k; m++) {
                float dist = color_distance_sq(colors + 3 * i, means[m]);
                if (dist < best_dist) { best_dist = dist; best = m; }
            }
            for (int ch = 0; ch < 3; ch++) sums[best][ch] += colors[3 * i + ch];
            sizes[best]++;
        }
        for (int m = 0; m < k; m++)
            if (sizes[m])
                for (int ch = 0; ch < 3; ch++) means[m][ch] = (float)(sums[m][ch] / sizes[m]);
    }
    return k;
}

static void build_palette(const float *colors, int n, struct analysis *out) {
    float means[CLUSTER_COUNT][3];
    int sizes[CLUSTER_COUNT];
    int k = kmeans(colors, n, means, sizes);

    float score[CLUSTER_COUNT];
    for (int m = 0; m < k; m++) {
        float h, s, v;
        rgb_to_hsv(means[m], &h, &s, &v);
        score[m] = sizes[m] * (0.05f + s * s);
    }

    for (int allow_grey = 0; allow_grey < 2 && out->n_palette == 0; allow_grey++) {
        int used[CLUSTER_COUNT] = {0};
        while (out->n_palette < MAX_PALETTE) {
            int best = -1;
            for (int m = 0; m < k; m++)
                if (!used[m] && sizes[m] && (best < 0 || score[m] > score[best])) best = m;
            if (best < 0) break;
            used[best] = 1;

            float h, s, v, vivid[3];
            rgb_to_hsv(means[best], &h, &s, &v);
            if (!allow_grey && s < 0.15f) continue;
            hsv_to_rgb(h, fminf(1, s * 1.1f), fmaxf(v, 0.88f), vivid);

            int duplicate = 0;
            for (int p = 0; p < out->n_palette; p++)
                if (color_distance_sq(vivid, out->palette[p]) < 0.02f) duplicate = 1;
            if (!duplicate) memcpy(out->palette[out->n_palette++], vivid, sizeof vivid);
        }
    }

    if (out->n_palette == 0) {
        const float fallback[3] = {0.62f, 0.7f, 1};
        memcpy(out->palette[0], fallback, sizeof fallback);
        out->n_palette = 1;
    }
}

void analyze_image(const unsigned char *rgba, int w, int h, float ellipse, struct analysis *out) {
    memset(out, 0, sizeof *out);
    out->center_x = out->center_y = 0.5f;

    struct image img = downscale(rgba, w, h);
    int n = img.width * img.height;
    float background[3];
    border_color(&img, background);

    float *contrast = malloc(sizeof(float) * n), *scratch = malloc(sizeof(float) * n);
    float *colors = malloc(sizeof(float) * 3 * n);
    for (int i = 0; i < n; i++) scratch[i] = contrast[i] = sqrtf(color_distance_sq(img.rgb + 3 * i, background));
    float threshold = fmaxf(0.12f, quantile(scratch, n, 0.92f));

    double sum_x = 0, sum_y = 0, sum_w = 0;
    int subject_pixels = 0;
    for (int i = 0; i < n; i++) {
        if (contrast[i] < threshold) continue;
        double weight = contrast[i] - threshold + 0.02;
        sum_x += weight * (i % img.width + 0.5);
        sum_y += weight * (i / img.width + 0.5);
        sum_w += weight;
        scratch[subject_pixels++] = contrast[i];
    }

    int n_colors = 0;
    if (subject_pixels > 4 && subject_pixels >= n / 500) {
        out->center_x = (float)(sum_x / sum_w / img.width);
        out->center_y = (float)(sum_y / sum_w / img.height);
        float strong = quantile(scratch, subject_pixels, 0.5f);

        int r = 0;
        for (int i = 0; i < n; i++) {
            if (contrast[i] < threshold) continue;
            float dx = ((i % img.width + 0.5f) / img.width - out->center_x) * w / h / ellipse;
            float dy = (i / img.width + 0.5f) / img.height - out->center_y;
            scratch[r++] = sqrtf(dx * dx + dy * dy);
            if (contrast[i] >= strong) memcpy(colors + 3 * n_colors++, img.rgb + 3 * i, 3 * sizeof(float));
        }
        out->radius = fmaxf(0.04f, quantile(scratch, r, SUBJECT_QUANTILE));
        out->found_subject = out->radius <= MAX_SUBJECT_RADIUS;
    }

    if (!out->found_subject) {
        out->center_x = out->center_y = 0.5f;
        out->radius = 0;
        n_colors = 0;
        for (int i = 0; i < n; i += 2) memcpy(colors + 3 * n_colors++, img.rgb + 3 * i, 3 * sizeof(float));
    }
    build_palette(colors, n_colors, out);

    free(img.rgb);
    free(contrast);
    free(scratch);
    free(colors);
}
