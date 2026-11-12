// audio_common.c — the platform-independent half: playback ring with markers, WAV injection, sfx lookup.
// The platform file (audio_esp.c / audio_host.c) pulls frames with audio_pull() and pushes mic frames
// with audio_push_capture().
#define LOG_TAG "audio"
#include "audio/audio.h"
#include "audio_internal.h"
#include "audio_loud.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/ringbuf.h"
#include "foundation/wav.h"
#include "foundation/atomic_file.h"
#include "foundation/clock.h"
#include "foundation/task.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>

#define PLAY_RING_SECONDS 30
#define MAX_MARKERS 32

static ringbuf_t s_ring;
static uint8_t *s_ring_mem;
static mutex_t s_lock;
static struct { int id; uint64_t at; } s_markers[MAX_MARKERS];
static size_t s_marker_n;
static atomic_uint_fast64_t s_written, s_played;     // sample counters
static audio_marker_cb s_marker_cb; static void *s_marker_user;
// The volume is heard in even steps (2026-11-05, the owner wanted the volume on the pairing page): 100 is the loudest,
// every 10 below it is 3 dB quieter, so 10, the page's quietest, is 27 dB down; 0 is silence. It used to be a share of
// the amplitude, which put nearly all the ear hears into 0–30 (50 was only 6 dB below 100). Applied as the output
// pulls, so a change is heard at once, even in the middle of a sentence.
#define VOLUME_DB_PER_UNIT 0.3f
#define SELFTEST_VOLUME AUDIO_VOLUME_DEFAULT   // the self-test asks whether the speaker works, not how loud it is set
static atomic_int s_volume = AUDIO_VOLUME_DEFAULT;
static atomic_int s_gain = 32768;                    // Q15, from s_volume; 32768 leaves the sound as it is
static int volume_gain(int v) { return v <= 0 ? 0 : v >= 100 ? 32768 : (int)lroundf(32768.0f * powf(10.0f, (float)(v - 100) * VOLUME_DB_PER_UNIT / 20.0f)); }
static atomic_int s_level;
static audio_loud_t s_loud;                          // the leveller and limiter (audio_loud.h), the output task's alone
static atomic_bool s_loud_on = true;
static audio_capture_cb s_cap_cb; static void *s_cap_user;
static atomic_bool s_cap_active;
static char s_sfx_dir[128] = "content/sfx";

// Effects over a song (2026-11-12, the owner: "触发它唱歌的时候，如果我再屏幕点击触发它交互……音乐会中断并出现奇怪异响").
// The song renders 1.2 s ahead into the one playback ring, so a tap's applause went in behind it: heard 1.2 s late, the
// song stopped for its 1.3 s, every beat after it late by as much, and the leveller lifting the quiet clap like a quiet
// voice until the song slammed back in. While a song plays, an effect is mixed over it as the output pulls instead: heard
// at once, every sample of the song where it was. It is lifted so its peak reaches MIX_PEAK (the clap is quiet on file;
// alone, the leveller lifts it). Everywhere else an effect stays in line: a [coo] in a sentence is heard where it was said.
#define MIX_SAMPLES   (AUDIO_SAMPLE_RATE * 2)
#define MIX_PEAK      14000
#define MIX_MAX_LIFT  6
static int16_t *s_mix;                               // PSRAM ring: s_mix_n samples still to be heard, from s_mix_at
static size_t s_mix_at, s_mix_n;
static atomic_bool s_mix_on;
static int16_t clamp16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

esp_err_t audio_common_init(const char *sfx_dir)
{
    s_lock = mutex_create();
    atomic_store(&s_gain, volume_gain(atomic_load(&s_volume)));
    audio_loud_init(&s_loud);
    s_ring_mem = mem_alloc_big(AUDIO_SAMPLE_RATE * 2 * PLAY_RING_SECONDS);
    if (!s_ring_mem) return ESP_ERR_NO_MEM;
    ringbuf_init(&s_ring, s_ring_mem, AUDIO_SAMPLE_RATE * 2 * PLAY_RING_SECONDS);
    s_mix = mem_alloc_big(MIX_SAMPLES * 2);
    if (!s_mix) return ESP_ERR_NO_MEM;
    if (sfx_dir) snprintf(s_sfx_dir, sizeof s_sfx_dir, "%s", sfx_dir);
    if (sfx_dir) {                                                 // the songs sit beside the sound effects: …/sfx → …/songs
        char songs[140]; snprintf(songs, sizeof songs, "%s", sfx_dir);
        char *tail = strrchr(songs, '/');
        if (tail) { snprintf(tail + 1, sizeof songs - (size_t)(tail + 1 - songs), "songs"); audio_song_set_dir(songs); }
    }
    return ESP_OK;
}

// ---- playback side ----
esp_err_t audio_play_pcm(const int16_t *pcm, size_t samples)
{
    mutex_lock(s_lock);
    size_t wrote = ringbuf_write(&s_ring, pcm, samples * 2) / 2;
    atomic_fetch_add(&s_written, wrote);
    mutex_unlock(s_lock);
    return wrote == samples ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t audio_play_marker(int id)
{
    mutex_lock(s_lock);
    if (s_marker_n < MAX_MARKERS) { s_markers[s_marker_n].id = id; s_markers[s_marker_n].at = atomic_load(&s_written); s_marker_n++; }
    mutex_unlock(s_lock);
    return ESP_OK;
}

void audio_play_flush(void)
{
    mutex_lock(s_lock);
    ringbuf_clear(&s_ring);
    atomic_store(&s_played, atomic_load(&s_written));
    s_marker_n = 0;
    s_mix_n = 0; atomic_store(&s_mix_on, false);
    mutex_unlock(s_lock);
}

static esp_err_t mix_pcm(const int16_t *pcm, size_t n)        // over what is playing, from the next pull on
{
    if (n > MIX_SAMPLES) n = MIX_SAMPLES;
    int peak = 1;
    for (size_t i = 0; i < n; i++) { int a = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (a > peak) peak = a; }
    int lift = MIX_PEAK * 256 / peak; if (lift > MIX_MAX_LIFT * 256) lift = MIX_MAX_LIFT * 256;   // Q8
    mutex_lock(s_lock);
    for (size_t i = 0; i < n; i++) {                           // a second tap's applause joins the first's
        size_t k = (s_mix_at + i) % MIX_SAMPLES;
        s_mix[k] = clamp16((int32_t)pcm[i] * lift / 256 + (i < s_mix_n ? s_mix[k] : 0));
    }
    if (n > s_mix_n) s_mix_n = n;
    atomic_store(&s_mix_on, true);
    mutex_unlock(s_lock);
    return ESP_OK;
}

uint8_t audio_play_level(void) { return (uint8_t)atomic_load(&s_level); }
uint64_t audio_written_samples(void) { return atomic_load(&s_written); }
uint64_t audio_played_samples(void) { return atomic_load(&s_played); }
uint32_t audio_play_queued_ms(void) { return (uint32_t)(ringbuf_used(&s_ring) / 2 * 1000 / AUDIO_SAMPLE_RATE); }
bool audio_play_idle(void) { return ringbuf_used(&s_ring) == 0; }
void audio_set_marker_cb(audio_marker_cb cb, void *user) { s_marker_user = user; s_marker_cb = cb; }
void audio_set_volume(uint8_t percent) { int v = percent > 100 ? 100 : percent; atomic_store(&s_volume, v); atomic_store(&s_gain, volume_gain(v)); }
bool audio_loudness(bool on) { return atomic_exchange(&s_loud_on, on); }

// Called by the platform output path: fills `out` with up to `samples`, zero-padded, levelled and limited (audio_loud.h),
// then scaled by the volume, so every step of the volume is 3 dB whatever the leveller does. Fires markers whose position
// has been reached.
size_t audio_pull(int16_t *out, size_t samples)
{
    mutex_lock(s_lock);
    size_t got = ringbuf_read(&s_ring, out, samples * 2) / 2;
    uint64_t played = atomic_fetch_add(&s_played, got) + got;
    int fire[MAX_MARKERS]; size_t nf = 0;
    size_t keep = 0;
    for (size_t i = 0; i < s_marker_n; i++) {
        if (s_markers[i].at <= played) fire[nf++] = s_markers[i].id;
        else s_markers[keep++] = s_markers[i];
    }
    s_marker_n = keep;
    mutex_unlock(s_lock);
    uint64_t acc = 0;                                  // the voice's loudness for the beak: before the volume, so turning
    for (size_t i = 0; i < got; i++) acc += (uint64_t)((int32_t)out[i] * out[i]);   // Gugu down does not still it
    int rms = got ? (int)(__builtin_sqrt((double)(acc / got)) / 32.0) : 0;    // RMS / 32: Gugu's voice reads about 30, 8160 and up 255
    atomic_store(&s_level, rms > 255 ? 255 : rms);
    if (got < samples) memset(out + got, 0, (samples - got) * 2);
    if (atomic_load(&s_mix_on)) {                      // the effects over the song: after the beak's level, which is Gugu's alone
        mutex_lock(s_lock);
        size_t k = s_mix_n < samples ? s_mix_n : samples;
        for (size_t i = 0; i < k; i++) out[i] = clamp16((int32_t)out[i] + s_mix[(s_mix_at + i) % MIX_SAMPLES]);
        s_mix_at = (s_mix_at + k) % MIX_SAMPLES; s_mix_n -= k;
        if (!s_mix_n) atomic_store(&s_mix_on, false);
        mutex_unlock(s_lock);
    }
    if (atomic_load(&s_loud_on)) audio_loud_run(&s_loud, out, samples);   // the padding too: the look-ahead drains through it
    int g = atomic_load(&s_gain);
    if (g < 32768) for (size_t i = 0; i < samples; i++) out[i] = (int16_t)((int32_t)out[i] * g / 32768);
    for (size_t i = 0; i < nf; i++) if (s_marker_cb) s_marker_cb(fire[i], s_marker_user);
    return got;
}

// ---- capture side ----
static atomic_uint s_cap_samples; static atomic_int s_cap_peak; static atomic_uint_fast64_t s_cap_sq;

esp_err_t audio_capture_start(audio_capture_cb cb, void *user)
{
    s_cap_user = user; s_cap_cb = cb;
    atomic_store(&s_cap_samples, 0); atomic_store(&s_cap_peak, 0); atomic_store(&s_cap_sq, 0);
    atomic_store(&s_cap_active, true);
    return audio_platform_capture(true);
}

void audio_capture_stats(audio_capture_stats_t *out)
{
    unsigned n = atomic_load(&s_cap_samples); uint64_t sq = atomic_load(&s_cap_sq);
    out->ms = (uint32_t)((uint64_t)n * 1000 / AUDIO_SAMPLE_RATE);
    out->peak = atomic_load(&s_cap_peak);
    out->rms = n ? (int)__builtin_sqrt((double)(sq / n)) : 0;
}

static atomic_int s_delivering;   // a capture callback is running (deliver_capture)

void audio_capture_stop(void)
{
    atomic_store(&s_cap_active, false);
    audio_platform_capture(false);
    while (atomic_load(&s_delivering)) clock_sleep_ms(1);   // a frame on its way finishes first: the caller hands the recording on next
}

bool audio_capture_active(void) { return atomic_load(&s_cap_active); }

static atomic_bool s_injecting;   // a replay is standing in for the microphone: live frames are dropped meanwhile

static void deliver_capture(const int16_t *pcm, size_t samples)
{
    atomic_fetch_add(&s_delivering, 1);
    audio_capture_cb cb = s_cap_cb;
    if (!atomic_load(&s_cap_active) || !cb) { atomic_fetch_sub(&s_delivering, 1); return; }
    int peak = atomic_load(&s_cap_peak); uint64_t sq = 0;
    for (size_t i = 0; i < samples; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
        sq += (uint64_t)((int32_t)pcm[i] * pcm[i]);
    }
    atomic_store(&s_cap_peak, peak); atomic_fetch_add(&s_cap_sq, sq); atomic_fetch_add(&s_cap_samples, (unsigned)samples);
    cb(pcm, samples, s_cap_user);
    atomic_fetch_sub(&s_delivering, 1);
}

void audio_push_capture(const int16_t *pcm, size_t samples)
{
    if (atomic_load(&s_injecting)) return;
    deliver_capture(pcm, samples);
}

esp_err_t audio_capture_inject_wav(const char *path, bool realtime)
{
    int64_t t0 = clock_us();
    atomic_store(&s_injecting, true);          // from here on the recording is the microphone
    size_t n; char *buf = file_read_all(path, &n);
    if (!buf) { atomic_store(&s_injecting, false); return ESP_ERR_NOT_FOUND; }
    uint32_t sr, bytes; uint16_t ch; size_t off;
    if (!wav_parse_header((uint8_t *)buf, n, &sr, &ch, &bytes, &off) || sr != AUDIO_SAMPLE_RATE || ch != 1) {
        LOGE("inject: %s is not 16 kHz mono 16-bit", path); mem_release(buf); atomic_store(&s_injecting, false); return ESP_ERR_INVALID_ARG;
    }
    int64_t t_read = clock_us() - t0;
    const int16_t *pcm = (const int16_t *)(buf + off);
    size_t total = bytes / 2;
    if (off + bytes > n) total = (n - off) / 2;
    for (size_t i = 0; i < total; i += AUDIO_FRAME_SAMPLES) {
        size_t k = total - i < AUDIO_FRAME_SAMPLES ? total - i : AUDIO_FRAME_SAMPLES;
        deliver_capture(pcm + i, k);
        if (realtime) clock_sleep_ms(20);
    }
    atomic_store(&s_injecting, false);
    mem_release(buf);
    LOGI("injected %u ms from %s (file read %lld ms, fed %lld ms)", (unsigned)(total * 1000 / AUDIO_SAMPLE_RATE), path,
         (long long)(t_read / 1000), (long long)((clock_us() - t0 - t_read) / 1000));
    return ESP_OK;
}

// ---- sfx ----
static esp_err_t play_wav(const char *path, bool over)
{
    size_t n; char *buf = file_read_all(path, &n);
    if (!buf) { LOGW("sound %s missing", path); return ESP_ERR_NOT_FOUND; }
    uint32_t sr, bytes; uint16_t ch; size_t off;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (wav_parse_header((uint8_t *)buf, n, &sr, &ch, &bytes, &off) && sr == AUDIO_SAMPLE_RATE && ch == 1) {
        size_t total = bytes / 2; if (off + bytes > n) total = (n - off) / 2;
        err = over ? mix_pcm((const int16_t *)(buf + off), total) : audio_play_pcm((const int16_t *)(buf + off), total);
    }
    mem_release(buf);
    return err;
}

esp_err_t audio_sfx_play(const char *name)                     // over a song, in line with anything else
{
    char path[200]; snprintf(path, sizeof path, "%s/%s.wav", s_sfx_dir, name);
    return play_wav(path, audio_song_playing());
}

esp_err_t audio_play_file(const char *path) { return play_wav(path, false); }

// ---- self-test ----
static void selftest_cb(const int16_t *pcm, size_t n, void *user) { (void)pcm; (void)n; (void)user; }

esp_err_t audio_selftest(audio_selftest_t *out)
{
    memset(out, 0, sizeof *out);
    if (audio_capture_active()) return ESP_ERR_INVALID_STATE;
    out->volume = (uint8_t)atomic_load(&s_volume);
    audio_capture_start(selftest_cb, NULL);
    clock_sleep_ms(500);
    audio_capture_stats_t a; audio_capture_stats(&a);
    audio_capture_stop();
    out->ambient_ms = a.ms; out->ambient_peak = a.peak; out->ambient_rms = a.rms;
    audio_capture_start(selftest_cb, NULL);
    atomic_store(&s_gain, volume_gain(SELFTEST_VOLUME));   // the beep at its own level, however far the volume is turned down
    bool loud = audio_loudness(false);                     // and as it is on file, not as the leveller last left things
    out->sfx = audio_sfx_play("beep");
    clock_sleep_ms(450);
    audio_loudness(loud);
    atomic_store(&s_gain, volume_gain(atomic_load(&s_volume)));
    audio_capture_stats_t b; audio_capture_stats(&b);
    audio_capture_stop();
    out->tone_ms = b.ms; out->tone_peak = b.peak; out->tone_rms = b.rms;
    LOGI("selftest: volume %u, quiet %u ms peak %d rms %d, beep %u ms peak %d rms %d (%s)", out->volume, (unsigned)a.ms, a.peak, a.rms,
         (unsigned)b.ms, b.peak, b.rms, esp_err_to_name(out->sfx));
    return ESP_OK;
}
