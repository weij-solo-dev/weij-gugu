// test_audio.c — the volume the owner sets is heard in even steps: 100 is the loudest and every 10 below it is 3 dB
// quieter, down to 10, the page's quietest (27 dB below); 0 is silence. The self-test's beep keeps its own level, so
// "does the speaker work" never depends on how far the volume was turned down. A frame still on its way into a call
// when the key comes up is through before audio_capture_stop() returns.
#include "minitest.h"
#include "audio/audio.h"
#include "audio/audio_internal.h"
#include "foundation/clock.h"
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

static int voice_level(int volume, int16_t amplitude)   // a square wave of this amplitude, played and pulled at this volume
{
    audio_set_volume((uint8_t)volume);
    int16_t in[320], out[320];
    for (int i = 0; i < 320; i++) in[i] = (int16_t)(i % 2 ? amplitude : -amplitude);
    audio_play_flush();
    audio_play_pcm(in, 320);
    audio_pull(out, 320);
    return audio_play_level();
}

// The beak follows the voice, not the speaker (2026-11-06): the level was taken after the volume, so turning Gugu down
// also stilled the beak, and on a scale (RMS / 128) where Gugu's own voice read about 8 against a threshold of 28.
TEST(test_audio_level_is_the_voice_not_the_volume)
{
    audio_once();
    CHECK_EQ_INT(voice_level(100, 3000), voice_level(20, 3000));     // turned down 24 dB: the same voice, the same level
    CHECK_EQ_INT(voice_level(60, 3000), 3000 / 32);
    CHECK_EQ_INT(voice_level(100, 1024), 32);                        // a quiet voice (−30 dBFS) reads 32 of 255
    CHECK_EQ_INT(voice_level(100, 20000), 255);                      // loud is the top
    CHECK_EQ_INT(voice_level(100, 0), 0);
    audio_set_volume(AUDIO_VOLUME_DEFAULT);
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

// The gift key's show sings (audio_song.c): a song from content/songs is synthesised as the speaker drains, sung twice
// when under 24 s, every beat reported in order once it has been pulled, and loud but never
// clipped; Twinkle (28 s) is long enough to sing once, a made-up song (8 bars) is sung twice; an unknown song is
// refused; stop is at once.
static int beats_seen, beat_last = -1, beat_order_bad; static int beat_per_bar;
static void on_beat(int beat, int per_bar, void *u) { (void)u; if (beat != beat_last + 1) beat_order_bad++; beat_last = beat; beats_seen++; beat_per_bar = per_bar; }
static int pull_song(int *peak, double *rms)   // the speaker, as fast as the song renders: returns ms of audio pulled
{
    int16_t out[320]; long long n = 0; double sq = 0; *peak = 0;
    for (int idle = 0; idle < 400;) {
        size_t got = audio_pull(out, 320);
        if (!got) { if (!audio_song_playing()) idle += 40; usleep(1000); idle++; continue; }
        idle = 0;
        for (size_t i = 0; i < got; i++) { int a = abs(out[i]); if (a > *peak) *peak = a; sq += (double)out[i] * out[i]; }
        n += (long long)got;
    }
    *rms = n ? sqrt(sq / (double)n) : 0;
    return (int)(n * 1000 / 16000);
}
TEST(test_audio_song)
{
    audio_once(); audio_set_volume(100); audio_play_flush();
    beats_seen = 0; beat_last = -1; beat_order_bad = 0;
    CHECK(audio_song_play("twinkle", on_beat, NULL) == ESP_OK);
    CHECK(audio_song_play("twinkle", on_beat, NULL) == ESP_ERR_INVALID_STATE);   // one at a time
    int peak; double rms;
    int ms = pull_song(&peak, &rms);
    CHECK(!audio_song_playing());
    CHECK_EQ_INT(beats_seen, 48); CHECK_EQ_INT(beat_order_bad, 0); CHECK_EQ_INT(beat_per_bar, 4);
    CHECK(ms > 27500 && ms < 29000);                          // 48 beats at 104 a minute, and a little ring-out
    CHECK(peak < 32000 && peak > 8000);                       // loud, never clipped
    CHECK(rms > 1500 && rms < 9000);
    beats_seen = 0; beat_last = -1;
    CHECK(audio_song_play("improvise", on_beat, NULL) == ESP_OK);
    pull_song(&peak, &rms);
    CHECK_EQ_INT(beats_seen, 64);
    beats_seen = 0; beat_last = -1;
    CHECK(audio_song_play("lullaby", on_beat, NULL) == ESP_OK); // 3/4, sung twice in its own key
    pull_song(&peak, &rms);
    CHECK_EQ_INT(beats_seen, 50); CHECK_EQ_INT(beat_per_bar, 3);
    CHECK(audio_song_play("no_such_song", on_beat, NULL) == ESP_ERR_NOT_FOUND);
    CHECK(audio_song_play("jingle_bells", on_beat, NULL) == ESP_OK);
    int16_t out[320]; for (int i = 0; i < 20; i++) { audio_pull(out, 320); usleep(2000); }
    int64_t t0 = clock_us();
    audio_song_stop();
    CHECK(!audio_song_playing() && clock_us() - t0 < 300000);  // stopped at once
    CHECK_EQ_INT(audio_play_queued_ms(), 0);                   // its notes dropped
    audio_set_volume(AUDIO_VOLUME_DEFAULT);
}

void test_audio(void)
{
    RUN(test_audio_volume_steps);
    RUN(test_audio_selftest_keeps_its_own_level);
    RUN(test_audio_level_is_the_voice_not_the_volume);
    RUN(test_audio_song);
    RUN(test_audio_capture_stop_waits_for_a_frame);
}
