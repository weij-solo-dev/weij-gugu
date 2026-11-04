// resample.c — see resample.h. The filter is designed once (windowed sinc, Hamming), gain 2 for the zeros inserted.
#include "foundation/resample.h"
#include <math.h>
#include <string.h>

#define TAPS 36
static float s_h[TAPS];
static int s_ready;

static void design(void)
{
    const double fc = 7400.0 / 48000.0, mid = (TAPS - 1) / 2.0;
    for (int k = 0; k < TAPS; k++) {
        double t = k - mid, sinc = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        double w = 0.54 - 0.46 * cos(2 * M_PI * k / (TAPS - 1));
        s_h[k] = (float)(2.0 * sinc * w);
    }
    s_ready = 1;
}

void resample_3to2_init(resample_3to2_t *r)
{
    if (!s_ready) design();
    memset(r, 0, sizeof *r);
}

static int16_t at(const resample_3to2_t *r, int64_t i)   // input number i, 0 before the start or out of the history
{
    int64_t back = (int64_t)r->n_in - 1 - i;
    if (i < 0 || back < 0 || back >= RESAMPLE_HIST) return 0;
    return r->hist[RESAMPLE_HIST - 1 - back];
}

size_t resample_3to2(resample_3to2_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t w = 0;
    for (size_t s = 0; s < n; s++) {
        memmove(r->hist, r->hist + 1, (RESAMPLE_HIST - 1) * sizeof(int16_t));
        r->hist[RESAMPLE_HIST - 1] = in[s];
        r->n_in++;
        while (3ull * r->m_out <= 2ull * (r->n_in - 1)) {      // output m sits at 48 kHz index 3m: its newest input is 3m/2
            int64_t j = 3ll * r->m_out;
            float acc = 0;
            for (int k = (int)(j & 1); k < TAPS; k += 2) acc += s_h[k] * at(r, (j - k) / 2);   // only even 48 kHz indices carry a sample
            int v = (int)lrintf(acc);
            out[w++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            r->m_out++;
        }
    }
    return w;
}
