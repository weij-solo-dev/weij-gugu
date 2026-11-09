// audio_loud.c — the leveller and the look-ahead limiter on the speaker's way out (audio_loud.h says why).
#include "audio_loud.h"
#include "audio/audio.h"
#include <math.h>
#include <string.h>

#define GATE_MS     3.1623e-5f   // −45 dBFS: quieter than this is silence, which leaves the leveller alone
#define TAU_S       1.0f         // the leveller listens over about a second
#define RATE_DB_S   15.0f        // and moves its gain at most this fast
#define ATTACK      (1.0f - 0.8187f)   // 1 − e^(−1/5): the limiter is down within the look-ahead (e^−6.4 left)
#define RELEASE     (1.0f / 1280.0f)   // and lets go over 80 ms

static float db_gain(float db) { return powf(10.0f, db / 20.0f); }

void audio_loud_init(audio_loud_t *s)
{
    memset(s, 0, sizeof *s);
    s->gain_db = AUDIO_LOUD_GAIN_START_DB;
    s->energy = powf(10.0f, (AUDIO_LOUD_TARGET_DB - AUDIO_LOUD_GAIN_START_DB) / 10.0f);
    s->lim = 1.0f;
    for (int i = 0; i < AUDIO_LOUD_LOOKAHEAD; i++) s->need[i] = 1.0f;
}

void audio_loud_run(audio_loud_t *s, int16_t *pcm, size_t n)
{
    if (!n) return;
    double sq = 0;                                             // 1. the leveller: how loud this frame is, if it sounds
    for (size_t i = 0; i < n; i++) sq += (double)pcm[i] * pcm[i];
    float ms = (float)(sq / (double)n / (32768.0 * 32768.0));
    if (ms > GATE_MS) {
        float k = (float)n / (TAU_S * AUDIO_SAMPLE_RATE);
        s->energy += (ms - s->energy) * (k > 1.0f ? 1.0f : k);
    }
    float want = AUDIO_LOUD_TARGET_DB - 10.0f * log10f(s->energy);
    if (want > AUDIO_LOUD_GAIN_MAX_DB) want = AUDIO_LOUD_GAIN_MAX_DB;
    if (want < AUDIO_LOUD_GAIN_MIN_DB) want = AUDIO_LOUD_GAIN_MIN_DB;
    float step = RATE_DB_S * (float)n / AUDIO_SAMPLE_RATE, g0 = s->gain_db;
    float g1 = want > g0 + step ? g0 + step : want < g0 - step ? g0 - step : want;
    s->gain_db = g1;
    float a0 = db_gain(g0), a1 = db_gain(g1);

    const float ceiling = (float)AUDIO_LOUD_CEILING;           // 2. per sample: the gain glides across the frame, the limiter looks ahead
    for (size_t i = 0; i < n; i++) {
        float x = (float)pcm[i] * (a0 + (a1 - a0) * (float)(i + 1) / (float)n), ax = fabsf(x);
        float out = s->delay[s->pos], m = s->need[s->pos];   // the sample leaving now counts too
        s->delay[s->pos] = x;
        s->need[s->pos] = ax > ceiling ? ceiling / ax : 1.0f;
        s->pos = (s->pos + 1) % AUDIO_LOUD_LOOKAHEAD;
        for (int k = 0; k < AUDIO_LOUD_LOOKAHEAD; k++) if (s->need[k] < m) m = s->need[k];
        s->lim += (m - s->lim) * (m < s->lim ? ATTACK : RELEASE);
        float y = out * s->lim;
        if (y > ceiling) y = ceiling;                          // what the glide left over
        if (y < -ceiling) y = -ceiling;
        pcm[i] = (int16_t)lrintf(y);
    }
}
