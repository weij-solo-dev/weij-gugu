// resample.h — 24 kHz → 16 kHz for the vendors whose voices come at 24 kHz: a streaming 3:2 polyphase converter
// (up 2, a 36-tap windowed-sinc low-pass at 48 kHz cutting near 7.4 kHz, down 3). No allocation; the state carries
// across chunks, so any split of the input gives the same output.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define RESAMPLE_HIST 20
typedef struct {
    int16_t hist[RESAMPLE_HIST];   // the newest inputs, oldest first
    uint32_t n_in, m_out;           // inputs seen, outputs made
} resample_3to2_t;

void   resample_3to2_init(resample_3to2_t *r);
// Converts n samples; out must hold n * 2 / 3 + 2. Returns the samples written.
size_t resample_3to2(resample_3to2_t *r, const int16_t *in, size_t n, int16_t *out);
