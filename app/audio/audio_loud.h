// audio_loud.h — private to audio: how loud the speaker is (2026-11-09, the owner: "现在 gugu 说话太小声了，即使10格也很小声").
// A leveller brings what is heard to one loudness whatever vendor spoke it (MiniMax's voice comes at about −25 dBFS
// while it sounds, with 9 dB of headroom left unused), and a look-ahead limiter keeps every sample under the ceiling,
// so a quiet voice can be lifted without a peak clipping. Silence holds the gain where it was: no pumping up of the
// quiet between sentences. Pure: no allocation, no locks; the output path runs it on each frame it pulls.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define AUDIO_LOUD_TARGET_DB     (-16.0f)   // the leveller's aim: the RMS of what is heard while it sounds
#define AUDIO_LOUD_CEILING       29205      // −1 dBFS: no sample goes past it
#define AUDIO_LOUD_LOOKAHEAD     32         // samples (2 ms at 16 kHz) the limiter sees coming
#define AUDIO_LOUD_GAIN_MAX_DB   14.0f
#define AUDIO_LOUD_GAIN_MIN_DB   (-8.0f)
#define AUDIO_LOUD_GAIN_START_DB 9.0f       // MiniMax's voice wants about this, so the first sentence after boot is right

typedef struct {
    float energy;                           // mean square (full scale 1.0) of what has sounded, averaged over about a second
    float gain_db;                          // the leveller's gain now
    float lim;                              // the limiter's gain now, ≤ 1
    float delay[AUDIO_LOUD_LOOKAHEAD];      // the samples on their way out, the leveller's gain applied
    float need[AUDIO_LOUD_LOOKAHEAD];       // the gain each of them needs to stay under the ceiling
    int pos;
} audio_loud_t;

void  audio_loud_init(audio_loud_t *s);
void  audio_loud_run(audio_loud_t *s, int16_t *pcm, size_t n);   // in place; 16 kHz; out lags in by the look-ahead
