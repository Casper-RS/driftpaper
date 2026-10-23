#pragma once

#define MUSIC_BANDS 16
#define MUSIC_RECENT_BEATS 4

// What is playing right now, all values 0..1. Bands run from bass to treble; `beat` jumps to 1 on
// a bass hit and decays; `presence` fades in while sound plays and out a moment after it stops.
// `beat_ages` are the seconds since the last few beats, newest first (large when there were none).
struct music_levels {
    float bands[MUSIC_BANDS];
    float level, bass, beat, presence;
    float beat_ages[MUSIC_RECENT_BEATS];
};

// Listens to what the speakers play (the monitor of the default output, or a named PulseAudio or
// PipeWire source) on a background thread. libpulse-simple is loaded only when this is called, so
// the wallpaper does not need it otherwise. Returns -1 when no sound server can be reached.
int music_start(const char *source);
void music_stop(void);
int music_running(void);
// Latest levels, or all zeros while stopped.
void music_read(struct music_levels *out);
// A fixed, plausible spectrum for still images (snapshots and previews).
void music_sample_levels(struct music_levels *out);
