#define _GNU_SOURCE
#include "music.h"
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Low rate and mono keep the listener far below the wallpaper's CPU budget; 5 kHz of spectrum is
// plenty to see a song move.
#define SAMPLE_RATE 11025
#define FFT_SIZE 512
#define HOP 256
#define BLOCK_SECONDS ((float)HOP / SAMPLE_RATE)
#define LOWEST_HZ 40.0f
#define HIGHEST_HZ 5000.0f
#define SILENT_RMS 0.0012f
#define PEAK_DECAY 0.997f
#define FADE_OUT_AFTER 1.5f
#define MIN_BEAT_GAP 0.2f
#define RETRY_SECONDS 5

// The few libpulse-simple declarations needed, so building needs no PulseAudio headers.
enum { PA_STREAM_RECORD = 2, PA_SAMPLE_FLOAT32LE = 5 };
typedef struct pa_simple pa_simple;
typedef struct { int format; uint32_t rate; uint8_t channels; } pa_sample_spec;
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } pa_buffer_attr;

static struct {
    pa_simple *(*connect)(const char *, const char *, int, const char *, const char *, const pa_sample_spec *,
                          const void *, const pa_buffer_attr *, int *);
    int (*read)(pa_simple *, void *, size_t, int *);
    void (*disconnect)(pa_simple *);
    const char *(*error_text)(int);
} pa;

struct listener {
    char source[256];
    atomic_int stop;
};

struct analyser {
    float history[FFT_SIZE], window[FFT_SIZE], re[FFT_SIZE], im[FFT_SIZE], cos_table[FFT_SIZE / 2], sin_table[FFT_SIZE / 2];
    int band_start[MUSIC_BANDS + 1];
    float band_peak[MUSIC_BANDS], loudest_rms, bass_average, since_beat, quiet_for;
    struct music_levels levels;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct listener *current;
static struct music_levels shared;

static int load_pulse(void) {
    static int loaded;
    if (loaded) return loaded > 0 ? 0 : -1;
    void *lib = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    if (lib) {
        pa.connect = dlsym(lib, "pa_simple_new");
        pa.read = dlsym(lib, "pa_simple_read");
        pa.disconnect = dlsym(lib, "pa_simple_free");
        pa.error_text = dlsym(lib, "pa_strerror");
    }
    loaded = lib && pa.connect && pa.read && pa.disconnect && pa.error_text ? 1 : -1;
    if (loaded < 0) fprintf(stderr, "driftpaper: music sync needs libpulse-simple.so.0 (PulseAudio or pipewire-pulse)\n");
    return loaded > 0 ? 0 : -1;
}

static void analyser_init(struct analyser *a) {
    memset(a, 0, sizeof *a);
    for (int i = 0; i < FFT_SIZE; i++) a->window[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (FFT_SIZE - 1));
    for (int i = 0; i < FFT_SIZE / 2; i++) {
        a->cos_table[i] = cosf(2 * (float)M_PI * i / FFT_SIZE);
        a->sin_table[i] = -sinf(2 * (float)M_PI * i / FFT_SIZE);
    }
    // Bands are spaced evenly in pitch, the way we hear them, and each covers at least one FFT bin.
    float hz_per_bin = (float)SAMPLE_RATE / FFT_SIZE;
    for (int b = 0; b <= MUSIC_BANDS; b++) {
        int bin = (int)(LOWEST_HZ * powf(HIGHEST_HZ / LOWEST_HZ, (float)b / MUSIC_BANDS) / hz_per_bin);
        a->band_start[b] = b && bin <= a->band_start[b - 1] ? a->band_start[b - 1] + 1 : bin;
    }
    a->since_beat = MIN_BEAT_GAP;
}

static void fft(struct analyser *a) {
    float *re = a->re, *im = a->im;
    for (int i = 1, j = 0; i < FFT_SIZE; i++) {
        int bit = FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= FFT_SIZE; len <<= 1) {
        int step = FFT_SIZE / len;
        for (int i = 0; i < FFT_SIZE; i += len)
            for (int k = 0; k < len / 2; k++) {
                float wr = a->cos_table[k * step], wi = a->sin_table[k * step];
                int p = i + k, q = p + len / 2;
                float tr = re[q] * wr - im[q] * wi, ti = re[q] * wi + im[q] * wr;
                re[q] = re[p] - tr; im[q] = im[p] - ti;
                re[p] += tr; im[p] += ti;
            }
    }
}

static float approach(float value, float target, float rise, float fall) {
    return value + (target - value) * (target > value ? rise : fall);
}

static void analyse(struct analyser *a, const float *block) {
    memmove(a->history, a->history + HOP, (FFT_SIZE - HOP) * sizeof(float));
    memcpy(a->history + FFT_SIZE - HOP, block, HOP * sizeof(float));

    float sum = 0;
    for (int i = 0; i < HOP; i++) sum += block[i] * block[i];
    float rms = sqrtf(sum / HOP);
    int quiet = rms < SILENT_RMS;
    struct music_levels *m = &a->levels;

    for (int i = 0; i < FFT_SIZE; i++) {
        a->re[i] = a->history[i] * a->window[i];
        a->im[i] = 0;
    }
    fft(a);

    float energy[MUSIC_BANDS], loudest_band = 1e-6f;
    for (int b = 0; b < MUSIC_BANDS; b++) {
        float power = 0;
        for (int k = a->band_start[b]; k < a->band_start[b + 1]; k++) power += a->re[k] * a->re[k] + a->im[k] * a->im[k];
        energy[b] = sqrtf(power / (a->band_start[b + 1] - a->band_start[b]));
        // Automatic gain: each band is measured against its own recent peak, so quiet songs and
        // treble still move, but never against less than 1/30 of the loudest band.
        if (!quiet) a->band_peak[b] = fmaxf(energy[b], a->band_peak[b] * PEAK_DECAY);
        loudest_band = fmaxf(loudest_band, a->band_peak[b]);
    }
    for (int b = 0; b < MUSIC_BANDS; b++) {
        float target = quiet ? 0 : fminf(1, energy[b] / fmaxf(a->band_peak[b], loudest_band * 0.033f));
        m->bands[b] = approach(m->bands[b], target, 0.7f, 0.16f);
    }

    if (!quiet) a->loudest_rms = fmaxf(rms, a->loudest_rms * PEAK_DECAY);
    m->level = approach(m->level, quiet ? 0 : fminf(1, rms / fmaxf(a->loudest_rms, 1e-6f)), 0.6f, 0.15f);

    // A beat is a sudden rise of the bass above its average of the last half second.
    float bass = (energy[0] + energy[1] + energy[2]) / 3, bass_peak = (a->band_peak[0] + a->band_peak[1] + a->band_peak[2]) / 3;
    float bass_share = quiet ? 0 : fminf(1, bass / fmaxf(bass_peak, 1e-6f));
    a->since_beat += BLOCK_SECONDS;
    if (!quiet && bass > a->bass_average * 1.35f && bass_share > 0.4f && a->since_beat >= MIN_BEAT_GAP) {
        m->beat = 1;
        a->since_beat = 0;
    } else {
        m->beat *= 0.88f;
    }
    a->bass_average = a->bass_average * 0.95f + bass * 0.05f;
    m->bass = approach(m->bass, bass_share, 0.6f, 0.12f);

    a->quiet_for = quiet ? a->quiet_for + BLOCK_SECONDS : 0;
    if (!quiet) m->presence = approach(m->presence, 1, 0.12f, 0);
    else if (a->quiet_for > FADE_OUT_AFTER) m->presence *= 0.95f;
}

static int publish(struct listener *me, const struct music_levels *levels) {
    pthread_mutex_lock(&lock);
    int still_current = current == me;
    if (still_current) shared = *levels;
    pthread_mutex_unlock(&lock);
    return still_current;
}

static void pause_unless_stopped(struct listener *me, int seconds) {
    for (int i = 0; i < seconds * 10 && !atomic_load(&me->stop); i++) usleep(100000);
}

// Each listener owns itself and frees itself when told to stop. music_stop() never waits for it,
// because a read can block for as long as the sound server sends nothing.
static void *listen_thread(void *data) {
    struct listener *me = data;
    struct analyser *a = malloc(sizeof *a);
    const pa_sample_spec spec = {PA_SAMPLE_FLOAT32LE, SAMPLE_RATE, 1};
    const pa_buffer_attr attr = {(uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, HOP * sizeof(float)};
    float block[HOP];
    int warned = 0;

    while (a && !atomic_load(&me->stop)) {
        int error = 0;
        analyser_init(a);
        pa_simple *stream = pa.connect(NULL, "driftpaper", PA_STREAM_RECORD, me->source, "Music sync", &spec, NULL, &attr, &error);
        if (!stream) {
            if (!warned++) fprintf(stderr, "driftpaper: cannot listen to '%s': %s; retrying\n", me->source, pa.error_text(error));
            pause_unless_stopped(me, RETRY_SECONDS);
            continue;
        }
        if (warned) fprintf(stderr, "driftpaper: listening to '%s' again\n", me->source);
        warned = 0;
        while (!atomic_load(&me->stop) && pa.read(stream, block, sizeof block, &error) == 0) {
            analyse(a, block);
            if (!publish(me, &a->levels)) break;
        }
        pa.disconnect(stream);
    }
    free(a);
    pthread_mutex_lock(&lock);
    if (current == me) current = NULL;
    pthread_mutex_unlock(&lock);
    free(me);
    return NULL;
}

int music_start(const char *source) {
    if (load_pulse()) return -1;
    music_stop();
    struct listener *me = calloc(1, sizeof *me);
    if (!me) return -1;
    snprintf(me->source, sizeof me->source, "%s", source && *source && strcmp(source, "default") ? source : "@DEFAULT_MONITOR@");

    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_mutex_lock(&lock);
    current = me;
    int failed = pthread_create(&thread, &attr, listen_thread, me);
    if (failed) current = NULL;
    pthread_mutex_unlock(&lock);
    pthread_attr_destroy(&attr);
    if (failed) {
        free(me);
        return -1;
    }
    return 0;
}

void music_stop(void) {
    pthread_mutex_lock(&lock);
    if (current) atomic_store(&current->stop, 1);
    current = NULL;
    memset(&shared, 0, sizeof shared);
    pthread_mutex_unlock(&lock);
}

int music_running(void) {
    pthread_mutex_lock(&lock);
    int running = current != NULL;
    pthread_mutex_unlock(&lock);
    return running;
}

void music_read(struct music_levels *out) {
    pthread_mutex_lock(&lock);
    *out = shared;
    pthread_mutex_unlock(&lock);
}

void music_sample_levels(struct music_levels *out) {
    static const float bands[MUSIC_BANDS] = {0.92f, 0.85f, 0.7f, 0.58f, 0.64f, 0.52f, 0.44f, 0.5f,
                                             0.47f, 0.38f, 0.42f, 0.34f, 0.29f, 0.26f, 0.21f, 0.17f};
    memcpy(out->bands, bands, sizeof bands);
    out->level = 0.6f;
    out->bass = 0.8f;
    out->beat = 0.5f;
    out->presence = 1;
}
