#define _GNU_SOURCE
#include "render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "stb_image.h"

#define TAU 6.2831853f
#define REFERENCE_HEIGHT 1440.0f
#define FLOATS_PER_PARTICLE 12

#define FRAGMENT_PRECISION \
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n" \
    "precision highp float;\n" \
    "#else\n" \
    "precision mediump float;\n" \
    "#endif\n"

// Positions are in "canvas units": canvas heights from the top-left corner, so a 16:9 canvas
// spans x 0..1.78 and y 0..1. The canvas is one monitor, or all monitors together with span.

static const char *BACKGROUND_VS =
    "attribute vec2 position;\n"
    "varying vec2 screen;\n"
    "void main() {\n"
    "    screen = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char *BACKGROUND_FS = FRAGMENT_PRECISION
    "varying vec2 screen;\n"
    "uniform sampler2D tex;\n"
    "uniform float time, aspect, fit, ellipse, fog, glow, pulse, brightness, contrast, saturation, opacity, music;\n"
    "uniform vec2 resolution, uv_scale, center, glow_falloff;\n"
    "uniform vec4 viewport;\n"
    "uniform vec3 letterbox, fog_a, fog_b, glow_a, glow_b, hue_adjust;\n"
    "\n"
    "float noise(vec2 p) {\n"
    "    p = mod(p, 251.0);\n"
    "    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "}\n"
    "\n"
    "float hue(vec3 c) {\n"
    "    float hi = max(c.r, max(c.g, c.b)), delta = hi - min(c.r, min(c.g, c.b));\n"
    "    if (delta <= 0.0) return 0.0;\n"
    "    if (hi == c.r) return 60.0 * mod((c.g - c.b) / delta, 6.0);\n"
    "    if (hi == c.g) return 60.0 * ((c.b - c.r) / delta + 2.0);\n"
    "    return 60.0 * ((c.r - c.g) / delta + 4.0);\n"
    "}\n"
    "\n"
    "vec3 adjust_image(vec3 c) {\n"
    "    c = (c - 0.5) * contrast + 0.5 + brightness;\n"
    "    c = mix(vec3(dot(c, vec3(0.299, 0.587, 0.114))), c, saturation);\n"
    "    if (hue_adjust.z != 0.0) {\n"
    "        float hue_distance = abs(mod(hue(c) - hue_adjust.x + 540.0, 360.0) - 180.0);\n"
    "        float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));\n"
    "        float weight = (1.0 - smoothstep(hue_adjust.y * 0.5, hue_adjust.y, hue_distance)) * smoothstep(0.05, 0.25, chroma);\n"
    "        c *= 1.0 + hue_adjust.z * weight;\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "\n"
    "vec3 drifting_fog(vec2 p, float falloff) {\n"
    "    float a = sin(p.x * 2.1 + time * 0.045 + sin(p.y * 1.7 - time * 0.03) * 1.6);\n"
    "    float b = sin(p.y * 2.6 - time * 0.038 + sin(p.x * 1.3 + time * 0.025) * 1.8);\n"
    "    return mix(fog_b, fog_a, a * b * 0.5 + 0.5) * (0.3 + 0.7 * falloff) * fog;\n"
    "}\n"
    "\n"
    "vec3 breathing_glow(float d2) {\n"
    "    float beat = 0.5 + 0.5 * sin(time * 0.6 * pulse);\n"
    "    return (glow_a * exp(-d2 * glow_falloff.x) * (0.10 + 0.08 * beat + 0.2 * music)\n"
    "          + glow_b * exp(-d2 * glow_falloff.y) * (0.035 + 0.02 * (1.0 - beat))) * glow;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 canvas = viewport.xy + screen * viewport.zw;\n"
    "    vec2 uv = (canvas - 0.5) * uv_scale + 0.5;\n"
    "    bool outside = uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0;\n"
    "    vec3 c = fit > 0.5 && outside ? letterbox : adjust_image(texture2D(tex, uv).rgb);\n"
    "    vec2 p = canvas * vec2(aspect, 1.0);\n"
    "    vec2 offset = (p - center) / vec2(ellipse, 1.0);\n"
    "    float d2 = dot(offset, offset);\n"
    "    if (fog > 0.0) c += drifting_fog(p, exp(-d2 * 2.2));\n"
    "    if (glow > 0.0) c += breathing_glow(d2);\n"
    "    c += (noise(floor(screen * resolution) + fract(time) * 17.0) - 0.5) / 255.0;\n"
    "    gl_FragColor = vec4(c, opacity);\n"
    "}\n";

// Per particle: shape = (u, v, size, kind), color = rgba, motion = (phase, speed, rotation, spin).
// Orbit uses u = angle and v = radius in image heights; the other motions use u, v as a 0..1 position.
static const char *PARTICLE_VS =
    "attribute vec4 shape, color, motion_params;\n"
    "uniform float time, aspect, radius_scale, inner, ellipse, motion, breathe, twinkle, opacity, music;\n"
    "uniform vec2 resolution, center;\n"
    "uniform vec4 viewport;\n"
    "varying vec4 tint;\n"
    "varying float kind, rotation;\n"
    "\n"
    "float phase() { return motion_params.x; }\n"
    "float speed() { return motion_params.y; }\n"
    "\n"
    "vec2 orbit(out float fade) {\n"
    "    float cycle = fract(time * 0.05 + phase() * 0.6667);\n"
    "    float radius = shape.y * radius_scale + (sin(time * 0.21 + phase()) * 0.47 + (cycle - 0.5) * 2.0) * breathe + music * 0.012;\n"
    "    fade = breathe > 0.0 ? smoothstep(0.0, 0.15, cycle) * smoothstep(1.0, 0.85, cycle) : 1.0;\n"
    "    float angle = shape.x + time * speed();\n"
    "    return center + vec2(cos(angle) * radius * ellipse, sin(angle) * radius);\n"
    "}\n"
    "\n"
    "vec2 vertical(float direction, out float fade) {\n"
    "    float y = fract(shape.y + direction * time * speed()) * 1.06 - 0.03;\n"
    "    fade = direction < 0.0 ? smoothstep(-0.03, 0.45, y) : 1.0;\n"
    "    return vec2(shape.x * aspect + sin(time * 0.6 + phase()) * 0.012, y);\n"
    "}\n"
    "\n"
    "vec2 wander() {\n"
    "    vec2 sway = vec2(sin(time * speed() + phase()), sin(time * speed() * 0.77 + phase() * 1.7));\n"
    "    return vec2(shape.x * aspect, shape.y) + sway * 0.05;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    float fade = 1.0;\n"
    "    vec2 p;\n"
    "    if (motion < 0.5) p = orbit(fade);\n"
    "    else if (motion < 1.5) p = vertical(-1.0, fade);\n"
    "    else if (motion < 2.5) p = vertical(1.0, fade);\n"
    "    else p = wander();\n"
    "\n"
    "    float clear_radius = inner * radius_scale;\n"
    "    fade *= smoothstep(clear_radius, clear_radius + 0.04, length((p - center) / vec2(ellipse, 1.0)));\n"
    "    float flicker = 1.0 - twinkle + twinkle * sin(time * (0.7 + fract(phase()) * 1.3) + phase() * 6.0);\n"
    "\n"
    "    tint = vec4(color.rgb, min(color.a * (1.0 + 0.6 * music), 1.0) * max(flicker, 0.0) * fade * opacity);\n"
    "    kind = shape.w;\n"
    "    rotation = motion_params.z + time * motion_params.w;\n"
    "    vec2 on_screen = (vec2(p.x / aspect, p.y) - viewport.xy) / viewport.zw;\n"
    "    gl_PointSize = shape.z * (1.0 + 0.4 * music) * resolution.y / viewport.w / 1440.0;\n"
    "    gl_Position = vec4(on_screen.x * 2.0 - 1.0, 1.0 - on_screen.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *PARTICLE_FS = FRAGMENT_PRECISION
    "varying vec4 tint;\n"
    "varying float kind, rotation;\n"
    "\n"
    "float band(float edge0, float edge1, float x) { return smoothstep(edge0 - 0.04, edge0 + 0.04, x) * (1.0 - smoothstep(edge1 - 0.04, edge1 + 0.04, x)); }\n"
    "float filled(float edge, float x) { return 1.0 - smoothstep(edge - 0.05, edge + 0.05, x); }\n"
    "\n"
    "float triangle(vec2 p) {\n"
    "    // Signed distance to an equilateral triangle (Inigo Quilez).\n"
    "    float k = sqrt(3.0);\n"
    "    p *= 1.25;\n"
    "    p.x = abs(p.x) - 1.0;\n"
    "    p.y += 1.0 / k;\n"
    "    if (p.x + k * p.y > 0.0) p = vec2(p.x - k * p.y, -k * p.x - p.y) / 2.0;\n"
    "    p.x -= clamp(p.x, -2.0, 0.0);\n"
    "    return 1.0 - smoothstep(-0.08, 0.08, -length(p) * sign(p.y));\n"
    "}\n"
    "float square(vec2 p) { vec2 q = abs(p); return band(0.66, 0.84, max(q.x, q.y)); }\n"
    "float spark(float r) { return exp(-r * r * 6.0) + 0.12 * exp(-r * r * 1.2); }\n"
    "float circle(float r) { return filled(0.67, r); }\n"
    "float ring(float r) { return band(0.56, 0.74, r); }\n"
    "float diamond(vec2 p) { return filled(0.67, abs(p.x) * 0.8 + abs(p.y)); }\n"
    "float star(vec2 p) { vec2 q = sqrt(abs(p)); return filled(0.9, q.x + q.y); }\n"
    "float plus(vec2 p) { vec2 q = abs(p); return filled(0.71, min(max(q.x, q.y * 5.0), max(q.x * 5.0, q.y))); }\n"
    "\n"
    "void main() {\n"
    "    vec2 p = gl_PointCoord * 2.0 - 1.0;\n"
    "    float c = cos(rotation), s = sin(rotation);\n"
    "    p = mat2(c, -s, s, c) * p;\n"
    "    float r = length(p);\n"
    "    float alpha;\n"
    "    if (kind < 0.5) alpha = triangle(p);\n"
    "    else if (kind < 1.5) alpha = square(p);\n"
    "    else if (kind < 2.5) alpha = spark(r);\n"
    "    else if (kind < 3.5) alpha = circle(r);\n"
    "    else if (kind < 4.5) alpha = ring(r);\n"
    "    else if (kind < 5.5) alpha = diamond(p);\n"
    "    else if (kind < 6.5) alpha = star(p);\n"
    "    else alpha = plus(p);\n"
    "    gl_FragColor = vec4(tint.rgb, tint.a * alpha);\n"
    "}\n";

// The music visualiser, drawn additively over the finished frame inside a scissor box. mode 0 is a
// ring of equaliser bars around the subject, 1 a glowing wave along the bottom, 2 the badge.
// Bands are mirrored so the picture stays symmetric: bass at the bottom of the ring and in the
// middle of the wave. Colours come out premultiplied: the badge's dark disc has alpha, glow has none.
static const char *MUSIC_FS = FRAGMENT_PRECISION
    "uniform float mode, time, aspect, radius, beat, bass, presence, opacity, size, bars, shadow, gain;\n"
    "uniform float bands[16];\n"
    "uniform vec2 resolution, center;\n"
    "uniform vec3 color_a, color_b, badge;\n"
    "uniform vec4 viewport;\n"
    "\n"
    "float band(float x) {\n"
    "    float v = 0.0;\n"
    "    for (int i = 0; i < 16; i++) v += bands[i] * (1.0 - smoothstep(0.0, 1.0, abs(x - float(i))));\n"
    "    return v;\n"
    "}\n"
    "\n"
    "vec3 tint(float x) { return mix(color_a, color_b, clamp(x / 15.0, 0.0, 1.0)); }\n"
    "float line(float distance, float width) { return 1.0 - smoothstep(0.0, width, abs(distance)); }\n"
    "\n"
    "vec4 ring(vec2 p, float px) {\n"
    "    vec2 offset = p - center;\n"
    "    float d = length(offset), angle = atan(offset.x, offset.y);\n"
    "    float x = abs(angle) / 3.14159265 * 15.0, v = band(x);\n"
    "    float reach = radius * 0.35, tip = radius + reach * v;\n"
    "    float slot = fract(angle / 6.2831853 * bars);\n"
    "    float bar = smoothstep(0.12, 0.22, slot) * smoothstep(0.88, 0.78, slot) * step(radius, d) * step(d, tip);\n"
    "    float rise = clamp((d - radius) / max(reach, 0.001), 0.0, 1.0);\n"
    "    float ripple_at = radius + (1.0 - beat) * radius * 0.45;\n"
    "    vec3 c = mix(tint(x), vec3(1.0), 0.3);\n"
    "    vec3 light = c * bar * (0.35 + 0.55 * rise)\n"
    "               + mix(c, vec3(1.0), 0.5) * (line(d - tip, 1.8 * px) + exp(-abs(d - tip) / (6.0 * px)) * 0.35)\n"
    "               + c * line(d - radius, 1.4 * px) * 0.55\n"
    "               + c * exp(-abs(d - ripple_at) / (5.0 * px)) * beat * beat * 0.7;\n"
    "    // A soft dark band behind the bars keeps them readable on bright pictures.\n"
    "    float shade = shadow * smoothstep(radius * 0.75, radius * 0.95, d) * (1.0 - smoothstep(tip, tip + radius * 0.45, d));\n"
    "    return vec4(light, shade);\n"
    "}\n"
    "\n"
    "vec4 wave(vec2 screen, float px) {\n"
    "    float x = abs(screen.x - 0.5) * 2.0 * 15.0, v = band(x);\n"
    "    float y = 1.0 - screen.y, crest = (0.07 + 0.11 * v) * size, echo = (0.07 + 0.07 * band(x * 0.8 + 2.0)) * size;\n"
    "    float column = fract(screen.x * resolution.x / (9.0 * px * resolution.y));\n"
    "    float columns = smoothstep(0.1, 0.25, column) * smoothstep(0.9, 0.75, column);\n"
    "    float below = step(y, crest) * pow(clamp(y / crest, 0.0, 1.0), 2.0);\n"
    "    vec3 c = tint(x);\n"
    "    vec3 light = c * below * columns * 0.3\n"
    "               + mix(c, vec3(1.0), 0.35) * (line(y - crest, 2.0 * px) * 0.9 + exp(-abs(y - crest) / (8.0 * px)) * 0.3)\n"
    "               + tint(15.0 - x) * line(y - echo, 1.5 * px) * 0.35;\n"
    "    return vec4(light * (0.75 + 0.5 * beat), 0.0);\n"
    "}\n"
    "\n"
    "float capsule(vec2 q, float half_height, float r) { return length(vec2(q.x, max(abs(q.y) - half_height, 0.0))) - r; }\n"
    "\n"
    "vec4 badge_icon(vec2 q, float r) {\n"
    "    float d = length(q), aa = 1.0;\n"
    "    float disc = 1.0 - smoothstep(r - aa, r + aa, d);\n"
    "    float breathing = 0.5 + 0.5 * sin(time * 1.6);\n"
    "    float alive = presence;\n"
    "    vec3 light = mix(color_a, vec3(1.0), 0.25) * line(d - r, 1.4) * (0.35 + 0.65 * max(bass, (1.0 - alive) * breathing * 0.4));\n"
    "    float ripple_at = r + (1.0 - beat) * r * 0.7;\n"
    "    light += color_a * line(d - ripple_at, 1.6) * beat * beat * (1.0 - smoothstep(r, r * 1.7, d)) * 0.9;\n"
    "    for (int i = 0; i < 5; i++) {\n"
    "        float fi = float(i), level = band(abs(fi - 2.0) * 5.0 + 0.5);\n"
    "        float half_height = r * mix(0.04 + 0.05 * breathing, 0.1 + 0.38 * level, alive);\n"
    "        float s = capsule(q - vec2((fi - 2.0) * r * 0.27, 0.0), half_height, r * 0.07);\n"
    "        vec3 c = mix(color_a, color_b, clamp(0.5 + q.y / (r * 1.2), 0.0, 1.0));\n"
    "        light += mix(c, vec3(1.0), 0.2) * (1.0 - smoothstep(-aa, aa, s)) * (0.55 + 0.45 * alive);\n"
    "    }\n"
    "    float shade = disc * 0.5;\n"
    "    return vec4(vec3(0.02, 0.022, 0.03) * shade + light * disc + light * (1.0 - disc) * 0.6, shade);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 screen = vec2(gl_FragCoord.x / resolution.x, 1.0 - gl_FragCoord.y / resolution.y);\n"
    "    vec4 c;\n"
    "    if (mode > 1.5) c = badge_icon(gl_FragCoord.xy - badge.xy, badge.z);\n"
    "    else {\n"
    "        float px = viewport.w / resolution.y;\n"
    "        vec2 canvas = viewport.xy + screen * viewport.zw;\n"
    "        c = mode < 0.5 ? ring(canvas * vec2(aspect, 1.0), px) : wave(screen, 1.0 / resolution.y);\n"
    "        c *= presence;\n"
    "    }\n"
    "    gl_FragColor = vec4(c.rgb * gain, c.a) * opacity;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "driftpaper: shader error: %s\n", log);
        return 0;
    }
    return shader;
}

static GLuint link_program(const char *vertex, const char *fragment, const char **attributes, int n_attributes) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex), fs = compile_shader(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    for (int i = 0; i < n_attributes; i++) glBindAttribLocation(program, i, attributes[i]);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        fprintf(stderr, "driftpaper: shader link error: %s\n", log);
        return 0;
    }
    return program;
}

static void find_uniforms(struct renderer *r) {
    GLuint p = r->background_program;
    r->bg = (struct background_uniforms){
        .tex = glGetUniformLocation(p, "tex"), .time = glGetUniformLocation(p, "time"),
        .aspect = glGetUniformLocation(p, "aspect"), .fit = glGetUniformLocation(p, "fit"),
        .ellipse = glGetUniformLocation(p, "ellipse"), .resolution = glGetUniformLocation(p, "resolution"),
        .uv_scale = glGetUniformLocation(p, "uv_scale"), .center = glGetUniformLocation(p, "center"),
        .letterbox = glGetUniformLocation(p, "letterbox"), .viewport = glGetUniformLocation(p, "viewport"),
        .opacity = glGetUniformLocation(p, "opacity"), .fog_a = glGetUniformLocation(p, "fog_a"),
        .fog_b = glGetUniformLocation(p, "fog_b"), .glow_a = glGetUniformLocation(p, "glow_a"),
        .glow_b = glGetUniformLocation(p, "glow_b"), .fog = glGetUniformLocation(p, "fog"),
        .glow = glGetUniformLocation(p, "glow"), .glow_falloff = glGetUniformLocation(p, "glow_falloff"),
        .pulse = glGetUniformLocation(p, "pulse"), .brightness = glGetUniformLocation(p, "brightness"),
        .contrast = glGetUniformLocation(p, "contrast"), .saturation = glGetUniformLocation(p, "saturation"),
        .hue_adjust = glGetUniformLocation(p, "hue_adjust"), .music = glGetUniformLocation(p, "music"),
    };
    p = r->particle_program;
    r->pt = (struct particle_uniforms){
        .time = glGetUniformLocation(p, "time"), .aspect = glGetUniformLocation(p, "aspect"),
        .radius_scale = glGetUniformLocation(p, "radius_scale"), .inner = glGetUniformLocation(p, "inner"),
        .ellipse = glGetUniformLocation(p, "ellipse"), .motion = glGetUniformLocation(p, "motion"),
        .breathe = glGetUniformLocation(p, "breathe"), .twinkle = glGetUniformLocation(p, "twinkle"),
        .resolution = glGetUniformLocation(p, "resolution"), .center = glGetUniformLocation(p, "center"),
        .viewport = glGetUniformLocation(p, "viewport"), .opacity = glGetUniformLocation(p, "opacity"),
        .music = glGetUniformLocation(p, "music"),
    };
    p = r->music_program;
    r->mu = (struct music_uniforms){
        .mode = glGetUniformLocation(p, "mode"), .time = glGetUniformLocation(p, "time"),
        .aspect = glGetUniformLocation(p, "aspect"), .resolution = glGetUniformLocation(p, "resolution"),
        .viewport = glGetUniformLocation(p, "viewport"), .center = glGetUniformLocation(p, "center"),
        .radius = glGetUniformLocation(p, "radius"),
        .bands = glGetUniformLocation(p, "bands"), .beat = glGetUniformLocation(p, "beat"),
        .bass = glGetUniformLocation(p, "bass"), .presence = glGetUniformLocation(p, "presence"),
        .color_a = glGetUniformLocation(p, "color_a"), .color_b = glGetUniformLocation(p, "color_b"),
        .badge = glGetUniformLocation(p, "badge"), .opacity = glGetUniformLocation(p, "opacity"),
        .size = glGetUniformLocation(p, "size"), .bars = glGetUniformLocation(p, "bars"),
        .shadow = glGetUniformLocation(p, "shadow"), .gain = glGetUniformLocation(p, "gain"),
    };
}

int renderer_init(struct renderer *r) {
    memset(r, 0, sizeof *r);
    const char *background_attributes[] = {"position"};
    const char *particle_attributes[] = {"shape", "color", "motion_params"};
    r->background_program = link_program(BACKGROUND_VS, BACKGROUND_FS, background_attributes, 1);
    r->particle_program = link_program(PARTICLE_VS, PARTICLE_FS, particle_attributes, 3);
    r->music_program = link_program(BACKGROUND_VS, MUSIC_FS, background_attributes, 1);
    if (!r->background_program || !r->particle_program || !r->music_program) return -1;
    find_uniforms(r);

    // A single triangle larger than the screen covers it without a diagonal seam.
    static const float fullscreen_triangle[] = {-1, -1, 3, -1, -1, 3};
    glGenBuffers(1, &r->triangle_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->triangle_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof fullscreen_triangle, fullscreen_triangle, GL_STATIC_DRAW);
    return 0;
}

static struct timespec modification_time(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtim : (struct timespec){0};
}

static int load_image(struct image *img, const char *path, float ellipse) {
    int w, h, channels;
    unsigned char *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "driftpaper: cannot load image '%s': %s\n", path, stbi_failure_reason());
        return -1;
    }
    GLint max_size;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    if (w > max_size || h > max_size) {
        fprintf(stderr, "driftpaper: image %dx%d exceeds the GPU texture limit of %d px\n", w, h, max_size);
        stbi_image_free(pixels);
        return -1;
    }

    glGenTextures(1, &img->texture);
    glBindTexture(GL_TEXTURE_2D, img->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    analyze_image(pixels, w, h, ellipse, &img->analysis);
    stbi_image_free(pixels);

    snprintf(img->path, sizeof img->path, "%s", path);
    img->mtime = modification_time(path);
    img->ellipse = ellipse;
    img->width = w;
    img->height = h;
    return 0;
}

static int same_image(const struct image *img, const char *path, float ellipse, struct timespec mtime) {
    return img->refs > 0 && !strcmp(img->path, path) && img->ellipse == ellipse &&
           img->mtime.tv_sec == mtime.tv_sec && img->mtime.tv_nsec == mtime.tv_nsec;
}

static struct image *acquire_image(struct renderer *r, const char *path, float ellipse) {
    struct timespec mtime = modification_time(path);
    struct image *free_slot = NULL;
    for (int i = 0; i < MAX_IMAGES; i++) {
        struct image *img = &r->images[i];
        if (same_image(img, path, ellipse, mtime)) {
            img->refs++;
            return img;
        }
        if (!img->refs && !free_slot) free_slot = img;
    }
    if (!free_slot || load_image(free_slot, path, ellipse)) return NULL;
    free_slot->refs = 1;
    return free_slot;
}

static void release_image(struct image *img) {
    if (--img->refs > 0) return;
    glDeleteTextures(1, &img->texture);
    img->texture = 0;
}

static float random_unit(unsigned *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return ((*seed >> 8) & 0xffffff) / 16777216.0f;
}

static float random_between(unsigned *seed, float lo, float hi) {
    return lo + random_unit(seed) * (hi - lo);
}

static int pick_shape(const struct config *cfg, unsigned *seed) {
    float total = 0;
    for (int i = 0; i < SHAPE_COUNT; i++) total += cfg->shape_weight[i];
    float pick = random_unit(seed) * total;
    for (int i = 0; i < SHAPE_COUNT; i++) {
        if (pick < cfg->shape_weight[i]) return i;
        pick -= cfg->shape_weight[i];
    }
    return SHAPE_SPARK;
}

static float orbit_radius(const struct scene *s, unsigned *seed) {
    float r0 = s->inner_radius, r1 = s->outer_radius, u = random_unit(seed);
    // Inverse-CDF sampling: density proportional to r gives an even spread per area, r^2 favours the outside.
    switch (s->cfg.distribution) {
    case DIST_EVEN: return sqrtf(r0 * r0 + u * (r1 * r1 - r0 * r0));
    case DIST_OUTER: return cbrtf(r0 * r0 * r0 + u * (r1 * r1 * r1 - r0 * r0 * r0));
    default: return r0 + u * (r1 - r0);
    }
}

static float orbit_direction(enum direction dir, unsigned *seed) {
    if (dir == DIR_CW) return 1;
    if (dir == DIR_CCW) return -1;
    return random_unit(seed) < 0.5f ? -1 : 1;
}

static void place_particle(const struct scene *s, const float (*palette)[3], int n_palette, unsigned *seed, float *p) {
    const struct config *cfg = &s->cfg;
    float distance_out = 0;

    if (cfg->motion == MOTION_ORBIT) {
        float radius = orbit_radius(s, seed);
        distance_out = (radius - s->inner_radius) / (s->outer_radius - s->inner_radius);
        p[0] = random_unit(seed) * TAU;
        p[1] = radius;
        float speedup = 1 + (cfg->speed_falloff - 1) * distance_out;
        p[9] = orbit_direction(cfg->direction, seed) * 0.003f * cfg->speed * speedup * random_between(seed, 0.67f, 1.33f);
    } else {
        p[0] = random_unit(seed);
        p[1] = random_unit(seed);
        float base_speed = cfg->motion == MOTION_DRIFT ? 0.25f : 0.035f;
        p[9] = base_speed * cfg->speed * random_between(seed, 0.6f, 1.4f);
    }

    p[2] = random_between(seed, cfg->size_min, cfg->size_max);
    p[3] = (float)pick_shape(cfg, seed);
    if (random_unit(seed) < cfg->white) {
        const float near_white[3] = {1.0f, 0.94f, 0.95f};
        memcpy(p + 4, near_white, sizeof near_white);
    } else {
        memcpy(p + 4, palette[(int)(random_unit(seed) * n_palette) % n_palette], 3 * sizeof(float));
    }
    p[7] = random_between(seed, cfg->opacity_min, cfg->opacity_max);
    p[8] = random_unit(seed) * TAU;
    p[10] = random_unit(seed) * TAU;
    float spin_boost = cfg->motion == MOTION_ORBIT ? 0.5f + 1.5f * distance_out : 1.0f;
    p[11] = random_between(seed, -0.3f, 0.3f) * cfg->spin * spin_boost;
}

int scene_create(struct renderer *r, struct scene *s, const char *image_path, const struct config *cfg) {
    memset(s, 0, sizeof *s);
    s->image = acquire_image(r, image_path, cfg->ellipse);
    if (!s->image) return -1;
    const struct analysis *an = &s->image->analysis;
    s->cfg = *cfg;
    s->center[0] = cfg->center_auto ? an->center_x : cfg->center_x;
    s->center[1] = cfg->center_auto ? an->center_y : cfg->center_y;
    s->inner_radius = cfg->inner_auto ? an->radius : cfg->inner_radius;
    s->outer_radius = fmaxf(cfg->outer_radius, s->inner_radius + 0.02f);

    const float (*palette)[3] = cfg->colors_auto ? an->palette : cfg->colors;
    int n_palette = cfg->colors_auto ? an->n_palette : cfg->n_colors;
    const float *primary = cfg->glow_auto ? palette[0] : cfg->glow_color;
    const float *secondary = cfg->glow_auto ? palette[n_palette > 1 ? 1 : 0] : cfg->glow_color;
    memcpy(s->glow_a, primary, sizeof s->glow_a);
    memcpy(s->glow_b, secondary, sizeof s->glow_b);

    int count = cfg->count;
    if (count > 0) {
        float *particles = malloc(count * FLOATS_PER_PARTICLE * sizeof(float));
        unsigned seed = cfg->seed;
        for (int i = 0; i < count; i++) place_particle(s, palette, n_palette, &seed, particles + FLOATS_PER_PARTICLE * i);
        glGenBuffers(1, &s->particle_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s->particle_vbo);
        glBufferData(GL_ARRAY_BUFFER, count * FLOATS_PER_PARTICLE * sizeof(float), particles, GL_STATIC_DRAW);
        free(particles);
    }
    s->particle_count = count;
    return 0;
}

void scene_destroy(struct scene *s) {
    if (s->particle_vbo) glDeleteBuffers(1, &s->particle_vbo);
    if (s->image) release_image(s->image);
    memset(s, 0, sizeof *s);
}

struct view {
    int width, height;
    float aspect, uv_scale[2], center[2], radius_scale;
    struct viewport viewport;
};

// uv = (canvas - 0.5) * uv_scale + 0.5 maps the canvas onto the image for each scale mode.
static struct view compute_view(const struct scene *s, int width, int height, struct viewport viewport, float aspect) {
    struct view v = {.width = width, .height = height, .aspect = aspect, .uv_scale = {1, 1}, .viewport = viewport};
    float image_aspect = (float)s->image->width / s->image->height;
    int canvas_wider = aspect > image_aspect;
    if (s->cfg.scale == SCALE_FILL) {
        if (canvas_wider) v.uv_scale[1] = image_aspect / aspect;
        else v.uv_scale[0] = aspect / image_aspect;
    } else if (s->cfg.scale == SCALE_FIT) {
        if (canvas_wider) v.uv_scale[0] = aspect / image_aspect;
        else v.uv_scale[1] = image_aspect / aspect;
    }
    v.radius_scale = 1 / v.uv_scale[1];
    v.center[0] = ((s->center[0] - 0.5f) / v.uv_scale[0] + 0.5f) * aspect;
    v.center[1] = (s->center[1] - 0.5f) / v.uv_scale[1] + 0.5f;
    return v;
}

static void scaled_color(GLint location, const float *rgb, float factor) {
    glUniform3f(location, rgb[0] * factor, rgb[1] * factor, rgb[2] * factor);
}

static void draw_background(const struct renderer *r, const struct scene *s, const struct view *v, float time, float opacity,
                            float pulse) {
    const struct config *cfg = &s->cfg;
    const struct background_uniforms *u = &r->bg;
    float glow_radius = s->inner_radius * v->radius_scale;
    float has_glow = glow_radius > 0;

    if (opacity < 1) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    glUseProgram(r->background_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->image->texture);
    glUniform1i(u->tex, 0);
    glUniform1f(u->time, time);
    glUniform1f(u->aspect, v->aspect);
    glUniform1f(u->opacity, opacity);
    glUniform1f(u->fit, cfg->scale == SCALE_FIT);
    glUniform1f(u->ellipse, 1 + (cfg->ellipse - 1) * 0.75f);
    glUniform2f(u->resolution, v->width, v->height);
    glUniform2fv(u->uv_scale, 1, v->uv_scale);
    glUniform2fv(u->center, 1, v->center);
    glUniform4f(u->viewport, v->viewport.x, v->viewport.y, v->viewport.width, v->viewport.height);
    glUniform3fv(u->letterbox, 1, cfg->bg_color);
    scaled_color(u->fog_a, s->glow_a, 0.08f);
    scaled_color(u->fog_b, s->glow_b, 0.085f);
    scaled_color(u->glow_a, s->glow_a, 0.58f);
    scaled_color(u->glow_b, s->glow_b, 0.6f);
    glUniform1f(u->fog, cfg->fog);
    glUniform1f(u->glow, has_glow ? cfg->glow : 0);
    // Gaussian falloffs 1/(2 sigma^2), with sigma at 0.69x and 1.41x the clear radius.
    glUniform2f(u->glow_falloff, has_glow ? 1 / (2 * powf(0.69f * glow_radius, 2)) : 0,
                has_glow ? 1 / (2 * powf(1.41f * glow_radius, 2)) : 0);
    glUniform1f(u->pulse, cfg->pulse);
    glUniform1f(u->brightness, cfg->brightness);
    glUniform1f(u->contrast, cfg->contrast);
    glUniform1f(u->saturation, cfg->saturation);
    glUniform3f(u->hue_adjust, cfg->hue_center, cfg->hue_width, cfg->hue_lightness);
    glUniform1f(u->music, pulse);

    glBindBuffer(GL_ARRAY_BUFFER, r->triangle_vbo);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void draw_particles(const struct renderer *r, const struct scene *s, const struct view *v, float time, float opacity,
                           float pulse) {
    const struct config *cfg = &s->cfg;
    const struct particle_uniforms *u = &r->pt;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glUseProgram(r->particle_program);
    glUniform1f(u->time, time);
    glUniform1f(u->aspect, v->aspect);
    glUniform1f(u->opacity, opacity);
    glUniform1f(u->radius_scale, v->radius_scale);
    glUniform1f(u->inner, s->inner_radius);
    glUniform1f(u->ellipse, cfg->ellipse);
    glUniform1f(u->motion, (float)cfg->motion);
    glUniform1f(u->breathe, cfg->breathe / REFERENCE_HEIGHT);
    glUniform1f(u->twinkle, cfg->twinkle);
    glUniform1f(u->music, pulse);
    glUniform2f(u->resolution, v->width, v->height);
    glUniform2fv(u->center, 1, v->center);
    glUniform4f(u->viewport, v->viewport.x, v->viewport.y, v->viewport.width, v->viewport.height);

    glBindBuffer(GL_ARRAY_BUFFER, s->particle_vbo);
    for (int i = 0; i < 3; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, FLOATS_PER_PARTICLE * sizeof(float), (void *)(i * 4 * sizeof(float)));
    }
    glDrawArrays(GL_POINTS, 0, s->particle_count);
}

void render_draw(const struct renderer *r, const struct scene *s, int width, int height, struct viewport viewport,
                 float canvas_aspect, float time, float opacity, float pulse) {
    struct view v = compute_view(s, width, height, viewport, canvas_aspect);
    glViewport(0, 0, width, height);
    draw_background(r, s, &v, time, opacity, pulse);
    if (s->particle_count > 0) draw_particles(r, s, &v, time, opacity, pulse);
}

float render_music_pulse(const struct config *cfg, const struct music_levels *m) {
    return fminf(1.5f, m->presence * cfg->music_sensitivity * (0.65f * m->beat + 0.35f * m->bass));
}

// Scissor boxes are in framebuffer pixels with the origin at the bottom left.
static void scissor_to(int width, int height, float left, float top, float right, float bottom) {
    int x0 = (int)fmaxf(0, floorf(left)), x1 = (int)fminf(width, ceilf(right));
    int y0 = (int)fmaxf(0, floorf(height - bottom)), y1 = (int)fminf(height, ceilf(height - top));
    glScissor(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

// Ring radius in canvas heights.
#define RING_RADIUS 0.11f
#define BADGE_RADIUS 0.024f
#define BADGE_RIGHT 0.022f
#define BADGE_BOTTOM 0.065f

static void draw_music_pass(const struct music_uniforms *u, float mode) {
    glUniform1f(u->mode, mode);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void render_music(const struct renderer *r, const struct scene *s, int width, int height, struct viewport viewport,
                  float canvas_aspect, float time, const struct music_levels *m, int badge, float opacity) {
    const struct config *cfg = &s->cfg;
    const struct music_uniforms *u = &r->mu;
    struct view v = compute_view(s, width, height, viewport, canvas_aspect);
    float bands[MUSIC_BANDS];
    for (int i = 0; i < MUSIC_BANDS; i++) bands[i] = fminf(1.2f, m->bands[i] * cfg->music_sensitivity);
    // The ring sits in the middle of the canvas: detected subjects are often off-centre or too large.
    float radius = RING_RADIUS * cfg->music_size, center[2] = {v.aspect * 0.5f, 0.5f};

    glViewport(0, 0, width, height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glUseProgram(r->music_program);
    glUniform1f(u->time, time);
    glUniform1f(u->aspect, v.aspect);
    glUniform2f(u->resolution, width, height);
    glUniform4f(u->viewport, viewport.x, viewport.y, viewport.width, viewport.height);
    glUniform2fv(u->center, 1, center);
    glUniform1f(u->radius, radius);
    glUniform1fv(u->bands, MUSIC_BANDS, bands);
    glUniform1f(u->beat, m->beat);
    glUniform1f(u->bass, fminf(1, m->bass * cfg->music_sensitivity));
    glUniform1f(u->presence, m->presence);
    glUniform3fv(u->color_a, 1, cfg->music_colors_auto ? s->glow_a : cfg->music_colors[0]);
    glUniform3fv(u->color_b, 1, cfg->music_colors_auto ? s->glow_b : cfg->music_colors[1]);
    glUniform1f(u->size, cfg->music_size);
    glUniform1f(u->bars, cfg->music_bars);
    glUniform1f(u->shadow, cfg->music_shadow);
    glUniform1f(u->gain, cfg->music_brightness);
    glUniform1f(u->opacity, opacity);
    glBindBuffer(GL_ARRAY_BUFFER, r->triangle_vbo);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    if (m->presence > 0.01f && cfg->music_style == MUSIC_RING) {
        // The bars reach 0.42 radius out and the shadow fades out 0.45 radius beyond them.
        float reach = radius * 1.9f + 0.02f;
        float cx = (0.5f - viewport.x) / viewport.width * width;
        float cy = (0.5f - viewport.y) / viewport.height * height;
        float rx = reach / v.aspect / viewport.width * width, ry = reach / viewport.height * height;
        scissor_to(width, height, cx - rx, cy - ry, cx + rx, cy + ry);
        draw_music_pass(u, 0);
    } else if (m->presence > 0.01f && cfg->music_style == MUSIC_WAVE) {
        // The crest peaks near 0.2 of the height at size 1; leave room for its glow.
        scissor_to(width, height, 0, height * fmaxf(0, 1 - 0.25f * cfg->music_size), width, height);
        draw_music_pass(u, 1);
    }
    if (badge) {
        float badge_radius = BADGE_RADIUS * height;
        float x = width - BADGE_RIGHT * height - badge_radius, y = height - BADGE_BOTTOM * height - badge_radius;
        glUniform3f(u->badge, x, height - y, badge_radius);
        scissor_to(width, height, x - 2 * badge_radius, y - 2 * badge_radius, x + 2 * badge_radius, y + 2 * badge_radius);
        draw_music_pass(u, 2);
    }
    glDisable(GL_SCISSOR_TEST);
}
