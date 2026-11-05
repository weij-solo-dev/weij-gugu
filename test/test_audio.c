// test_audio.c — the volume the owner sets is heard in even steps: 100 is the loudest and every 10 below it is 3 dB
// quieter, down to 10, the page's quietest (27 dB below); 0 is silence. The self-test's beep keeps its own level, so
// "does the speaker work" never depends on how far the volume was turned down. A frame still on its way into a call
// when the key comes up is through before audio_capture_stop() returns.
#include "minitest.h"
#include "audio/audio.h"
#include "audio/audio_internal.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

static void audio_once(void) { static bool done; if (!done) { audio_common_init(GUGU_REPO_ROOT "/content/sfx"); done = true; } }

static int level_at(int volume, int16_t sample)   // a steady sample played and pulled back at this volume
{
    audio_set_volume((uint8_t)volume);
    int16_t in[160], out[160];
    for (int i = 0; i < 160; i++) in[i] = sample;
    audio_play_flush();
    audio_play_pcm(in, 160);
    return audio_pull(out, 160) == 160 ? out[80] : -99999;
}

TEST(test_audio_volume_steps)
{
    audio_once();
    CHECK_EQ_INT(level_at(100, 10000), 10000);                   // the loudest leaves the sound as it is
    for (int v = 90; v >= 10; v -= 10) {
        int want = (int)(10000 * pow(10, (v - 100) * 0.3 / 20));
        CHECK(abs(level_at(v, 10000) - want) <= 2);
        CHECK(abs(level_at(v, -10000) + want) <= 2);             // both halves of the wave alike
    }
    CHECK(abs(level_at(10, 10000) - 447) <= 2);                  // 27 dB below the loudest
    CHECK(abs(level_at(50, 10000) - 1778) <= 2);                 // halfway is 15 dB down (it was 6 dB as a share)
    for (int v = 100; v > 10; v -= 10) {                         // every step the same size to the ear: 3 dB
        double r = (double)level_at(v - 10, 10000) / level_at(v, 10000);
        CHECK(r > 0.700 && r < 0.716);
    }
    CHECK_EQ_INT(level_at(0, 10000), 0);                         // silence
    CHECK_EQ_INT(level_at(250, 10000), 10000);                   // past the top is the top
    audio_set_volume(AUDIO_VOLUME_DEFAULT);
}

// The output task, as on the device: pulls 20 ms at a time while the self-test runs, keeping the loudest sample.
static atomic_bool s_pulling; static atomic_int s_peak;
static void *puller(void *arg)
{
    (void)arg;
    int16_t f[320];
    while (atomic_load(&s_pulling)) {
        size_t n = audio_pull(f, 320);
        for (size_t i = 0; i < n; i++) if (abs(f[i]) > atomic_load(&s_peak)) atomic_store(&s_peak, abs(f[i]));
        usleep(20000);
    }
    return NULL;
}
static int selftest_beep_peak(int volume, audio_selftest_t *t)
{
    audio_set_volume((uint8_t)volume);
    audio_play_flush();
    atomic_store(&s_peak, 0); atomic_store(&s_pulling, true);
    pthread_t th; pthread_create(&th, NULL, puller, NULL);
    esp_err_t e = audio_selftest(t);
    atomic_store(&s_pulling, false); pthread_join(th, NULL);
    return e == ESP_OK ? atomic_load(&s_peak) : -1;
}

TEST(test_audio_selftest_keeps_its_own_level)
{
    audio_once();
    audio_selftest_t lo, hi;
    int quiet = selftest_beep_peak(10, &lo), loud = selftest_beep_peak(100, &hi);
    CHECK(quiet > 3000);                                         // the beep is plainly there, turned down or not
    CHECK(abs(quiet - loud) <= loud / 100);                      // and the same either way
    CHECK_EQ_INT(lo.volume, 10);                                 // the report says what the owner set
    CHECK_EQ_INT(hi.volume, 100);
    CHECK_EQ_INT(lo.sfx, ESP_OK);
    CHECK(abs(level_at(10, 10000) - 447) <= 2);                  // afterwards the owner's volume is back
    audio_set_volume(AUDIO_VOLUME_DEFAULT);
}


// The call hands the recording to the recognizer the moment the key is up (2026-11-05: first, not after the bookkeeping),
// so a microphone frame still being copied in must be through before audio_capture_stop() returns.
static atomic_int s_cb_started, s_cb_done;
static void slow_cb(const int16_t *pcm, size_t n, void *user) { (void)pcm; (void)n; (void)user; atomic_store(&s_cb_started, 1); usleep(50000); atomic_store(&s_cb_done, 1); }
static void *push_one(void *arg) { (void)arg; int16_t f[320] = {0}; audio_push_capture(f, 320); return NULL; }

TEST(test_audio_capture_stop_waits_for_a_frame)
{
    audio_once();
    atomic_store(&s_cb_started, 0); atomic_store(&s_cb_done, 0);
    audio_capture_start(slow_cb, NULL);
    pthread_t th; pthread_create(&th, NULL, push_one, NULL);
    for (int i = 0; i < 500 && !atomic_load(&s_cb_started); i++) usleep(1000);
    CHECK_EQ_INT(atomic_load(&s_cb_started), 1);
    audio_capture_stop();
    CHECK_EQ_INT(atomic_load(&s_cb_done), 1);                    // the frame was through before stop returned
    pthread_join(th, NULL);
}

void test_audio(void)
{
    RUN(test_audio_volume_steps);
    RUN(test_audio_selftest_keeps_its_own_level);
    RUN(test_audio_capture_stop_waits_for_a_frame);
}
