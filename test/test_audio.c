// test_audio.c — the volume the owner sets is heard in even steps: 100 is the loudest and every 10 below it is 3 dB
// quieter, down to 10, the page's quietest (27 dB below); 0 is silence. The self-test's beep keeps its own level, so
// "does the speaker work" never depends on how far the volume was turned down. A frame still on its way into a call
// when the key comes up is through before audio_capture_stop() returns. The speaker's loudness (audio_loud.c) lifts a
// quiet voice to one level, brings a loud one down to it, leaves silence alone and never lets a sample past −1 dBFS.
#include "minitest.h"
#include "audio/audio.h"
#include "audio/audio_internal.h"
#include "audio/audio_loud.h"
#include "foundation/clock.h"
#include "foundation/wav.h"
#include "foundation/atomic_file.h"
#include "foundation/mem.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

static void audio_once(void) { static bool done; if (!done) { audio_common_init(GUGU_REPO_ROOT "/content/sfx"); done = true; } }

static int level_at(int volume, int16_t sample)   // a steady sample played and pulled back at this volume (the volume alone)
{
    audio_set_volume((uint8_t)volume);
    bool loud = audio_loudness(false);
    int16_t in[160], out[160];
    for (int i = 0; i < 160; i++) in[i] = sample;
    audio_play_flush();
    audio_play_pcm(in, 160);
    int got = audio_pull(out, 160) == 160 ? out[80] : -99999;
    audio_loudness(loud);
    return got;
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

// A tap during the show (2026-11-12, the owner: "音乐会中断并出现奇怪异响"): the applause is mixed over the song at once,
// not queued 1.2 s behind it. The lullaby (no drums, so no noise) is pulled twice, once with a clap: before the clap the
// two are the same sample for sample, during it they differ by the clap lifted to MIX_PEAK, and after it they are the same
// again: not one sample of the song moved. Out of a song an effect stays in line.
#define SONG_FRAMES 250                                  // 5 s
static void pull_lullaby(int16_t *dst, int clap_at)     // the song's first 5 s; a clap after frame clap_at (-1: none)
{
    audio_play_flush();
    CHECK(audio_song_play("lullaby", NULL, NULL) == ESP_OK);
    for (int f = 0; f < SONG_FRAMES;) {
        if (audio_play_queued_ms() < 40) { usleep(500); continue; }   // the song renders ahead; pull whole frames of it
        if (f == clap_at) CHECK_EQ_INT(audio_sfx_play("clap"), ESP_OK);
        audio_pull(dst + f * 320, 320); f++;
    }
    audio_song_stop();
}
TEST(test_audio_effect_over_a_song)
{
    audio_once(); audio_set_volume(100);
    bool loud = audio_loudness(false);                  // the samples as mixed, not as levelled
    int16_t *a = malloc(SONG_FRAMES * 320 * 2), *b = malloc(SONG_FRAMES * 320 * 2);
    pull_lullaby(a, -1);
    pull_lullaby(b, 50);                                 // at 1 s
    int at = 50 * 320, first = -1, peak = 0, before = 0, after = 0;
    for (int i = 0; i < SONG_FRAMES * 320; i++) {
        int d = b[i] - a[i];
        if (i < at) before += d != 0;
        else if (i >= at + 20800) after += d != 0;
        else { if (d && first < 0) first = i - at; if (abs(d) > peak) peak = abs(d); }
    }
    CHECK_EQ_INT(before, 0);
    CHECK(first >= 0 && first < 320);                    // heard in the very next frame
    CHECK(peak > 11000 && peak <= 14100);                // lifted to about MIX_PEAK (a clamp at a song's peak takes a little)
    CHECK_EQ_INT(after, 0);                              // and the song is where it was
    free(a); free(b);

    audio_play_flush();                                  // no song: in line, after what is queued
    uint64_t w = audio_written_samples();
    CHECK_EQ_INT(audio_sfx_play("coo"), ESP_OK);
    CHECK_EQ_INT((int)(audio_written_samples() - w), 7040);
    audio_play_flush();
    audio_loudness(loud);
    audio_set_volume(AUDIO_VOLUME_DEFAULT);
}

// ---- the speaker's loudness (2026-11-09, the owner: "现在 gugu 说话太小声了，即使10格也很小声") ----
static int16_t *voice_fixture(size_t *n)   // MiniMax's voice as it arrives: about −25 dBFS while it sounds, peaks at −9 dBFS
{
    size_t len; char *buf = file_read_all(GUGU_REPO_ROOT "/test/fixtures/live_tts_zh.wav", &len);
    uint32_t sr, bytes; uint16_t ch; size_t off;
    if (!buf || !wav_parse_header((uint8_t *)buf, len, &sr, &ch, &bytes, &off) || sr != 16000 || ch != 1) { mem_release(buf); return NULL; }
    *n = (len - off) / 2 < bytes / 2 ? (len - off) / 2 : bytes / 2;
    int16_t *pcm = malloc(*n * 2); memcpy(pcm, buf + off, *n * 2); mem_release(buf);
    return pcm;
}
typedef struct { double active_db; int peak; int frames_over_ceiling; } loud_stats_t;
static loud_stats_t loudness_of(const int16_t *pcm, size_t n)   // RMS of the 20 ms frames that sound (−45 dBFS up), and the peak
{
    loud_stats_t r = { -120, 0, 0 }; double sq = 0; long cnt = 0;
    for (size_t i = 0; i + 320 <= n; i += 320) {
        double f = 0; for (int k = 0; k < 320; k++) { f += (double)pcm[i + k] * pcm[i + k]; if (abs(pcm[i + k]) > r.peak) r.peak = abs(pcm[i + k]); }
        if (f / 320 > 32768.0 * 32768.0 * 3.1623e-5) { sq += f; cnt += 320; }
    }
    for (size_t i = 0; i < n; i++) if (abs(pcm[i]) > AUDIO_LOUD_CEILING) { r.frames_over_ceiling++; break; }
    if (cnt) r.active_db = 10 * log10(sq / (double)cnt / (32768.0 * 32768.0));
    return r;
}
static void run_frames(audio_loud_t *l, int16_t *pcm, size_t n) { for (size_t i = 0; i < n; i += 320) audio_loud_run(l, pcm + i, n - i < 320 ? n - i : 320); }

TEST(test_audio_loud_lifts_a_quiet_voice)
{
    size_t n; int16_t *voice = voice_fixture(&n);
    CHECK(voice != NULL); if (!voice) return;
    loud_stats_t in = loudness_of(voice, n);
    CHECK(in.active_db < -23 && in.active_db > -28);              // the fixture is the quiet voice the owner heard
    audio_loud_t l; audio_loud_init(&l);
    int16_t *out = malloc(n * 2);
    memcpy(out, voice, n * 2); run_frames(&l, out, n);           // the first sentence after boot
    loud_stats_t first = loudness_of(out, n);
    CHECK(fabs(first.active_db - AUDIO_LOUD_TARGET_DB) < 2.5);   // already about right: the gain starts where MiniMax wants it
    memcpy(out, voice, n * 2); run_frames(&l, out, n);           // and once the leveller has listened
    loud_stats_t settled = loudness_of(out, n);
    CHECK(fabs(settled.active_db - AUDIO_LOUD_TARGET_DB) < 1.5);
    CHECK(settled.active_db - in.active_db > 7);                 // 7 dB and more louder than it came
    CHECK(settled.peak <= AUDIO_LOUD_CEILING && settled.peak > AUDIO_LOUD_CEILING * 3 / 4);   // up to the ceiling, never past it
    printf("    voice %.1f dBFS → %.1f (first) → %.1f dBFS, peak %d, gain %.1f dB\n", in.active_db, first.active_db, settled.active_db, settled.peak, (double)l.gain_db);
    free(out); free(voice);
}

TEST(test_audio_loud_brings_a_loud_voice_down)
{
    size_t n; int16_t *voice = voice_fixture(&n);
    CHECK(voice != NULL); if (!voice) return;
    for (size_t i = 0; i < n; i++) { int v = voice[i] * 4; voice[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }   // a vendor 12 dB hotter, clipping
    loud_stats_t in = loudness_of(voice, n);
    audio_loud_t l; audio_loud_init(&l);
    int16_t *out = malloc(n * 2);
    for (int pass = 0; pass < 2; pass++) { memcpy(out, voice, n * 2); run_frames(&l, out, n); }
    loud_stats_t settled = loudness_of(out, n);
    CHECK(fabs(settled.active_db - AUDIO_LOUD_TARGET_DB) < 1.5);
    CHECK(settled.peak <= AUDIO_LOUD_CEILING);
    CHECK(l.gain_db < 0);
    printf("    loud voice %.1f dBFS → %.1f dBFS, gain %.1f dB\n", in.active_db, settled.active_db, (double)l.gain_db);
    free(out); free(voice);
}

TEST(test_audio_loud_leaves_silence_alone)
{
    size_t n; int16_t *voice = voice_fixture(&n);
    CHECK(voice != NULL); if (!voice) return;
    audio_loud_t l; audio_loud_init(&l);
    run_frames(&l, voice, n);
    float g = l.gain_db;
    int16_t quiet[320]; int loud_after = 0;
    for (int f = 0; f < 500; f++) {                               // 10 s between sentences
        memset(quiet, 0, sizeof quiet);
        if (f % 7 == 0) for (int i = 0; i < 320; i++) quiet[i] = (int16_t)((i * 37 % 11) - 5);   // a breath of noise, −76 dBFS
        audio_loud_run(&l, quiet, 320);
        if (f > 0) for (int i = 0; i < 320; i++) if (abs(quiet[i]) > 30) loud_after++;
    }
    CHECK(l.gain_db == g);                                       // the gain waited where it was
    CHECK_EQ_INT(loud_after, 0);                                 // and the quiet stayed quiet
    free(voice);
}

TEST(test_audio_loud_catches_a_sudden_peak)
{
    audio_loud_t l; audio_loud_init(&l);
    int16_t f[320]; int peak = 0, over = 0;
    for (int k = 0; k < 100; k++) {                              // 2 s of a soft tone: the leveller climbs to its top
        for (int i = 0; i < 320; i++) f[i] = (int16_t)(300 * sin(2 * M_PI * 440 * (k * 320 + i) / 16000.0));
        audio_loud_run(&l, f, 320);
    }
    CHECK(l.gain_db > AUDIO_LOUD_GAIN_MAX_DB - 0.5f);
    for (int k = 0; k < 10; k++) {                               // then a full-scale square, at that gain
        for (int i = 0; i < 320; i++) f[i] = (i / 20) % 2 ? 32767 : -32768;
        audio_loud_run(&l, f, 320);
        for (int i = 0; i < 320; i++) { if (abs(f[i]) > peak) peak = abs(f[i]); if (abs(f[i]) > AUDIO_LOUD_CEILING) over++; }
    }
    CHECK_EQ_INT(over, 0);
    CHECK(peak > AUDIO_LOUD_CEILING * 9 / 10);                   // held at the ceiling, not shut off
}

TEST(test_audio_loud_on_the_way_out)
{
    audio_once(); audio_set_volume(100); audio_play_flush();
    size_t n; int16_t *voice = voice_fixture(&n);
    CHECK(voice != NULL); if (!voice) return;
    int16_t *out = malloc(n * 2);
    for (int pass = 0; pass < 2; pass++) {
        audio_play_pcm(voice, n);
        size_t got = 0; while (got < n) got += audio_pull(out + got, n - got < 320 ? n - got : 320);
    }
    loud_stats_t in = loudness_of(voice, n), heard = loudness_of(out, n);
    CHECK(heard.active_db - in.active_db > 7);                   // the speaker gets the lifted voice
    CHECK(heard.peak <= AUDIO_LOUD_CEILING);
    CHECK_EQ_INT(voice_level(100, 1024), 32);                    // the beak still reads the voice as it came
    audio_set_volume(AUDIO_VOLUME_DEFAULT);
    free(out); free(voice);
}

void test_audio(void)
{
    RUN(test_audio_loud_lifts_a_quiet_voice);
    RUN(test_audio_loud_brings_a_loud_voice_down);
    RUN(test_audio_loud_leaves_silence_alone);
    RUN(test_audio_loud_catches_a_sudden_peak);
    RUN(test_audio_loud_on_the_way_out);
    RUN(test_audio_volume_steps);
    RUN(test_audio_selftest_keeps_its_own_level);
    RUN(test_audio_level_is_the_voice_not_the_volume);
    RUN(test_audio_song);
    RUN(test_audio_effect_over_a_song);
    RUN(test_audio_capture_stop_waits_for_a_frame);
}
