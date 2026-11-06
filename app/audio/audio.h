// audio.h — microphone capture (push-to-talk), PCM playback with a jitter buffer and markers, local
// sound effects. 16 kHz mono 16-bit everywhere. Device: ES8311 over I2S; host: SDL audio.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_FRAME_SAMPLES 320          // 20 ms

esp_err_t audio_init(void);

// ---- capture ----
typedef void (*audio_capture_cb)(const int16_t *pcm, size_t samples, void *user);   // from the audio task
esp_err_t audio_capture_start(audio_capture_cb cb, void *user);
void      audio_capture_stop(void);
bool      audio_capture_active(void);
// Test / replay: feed a 16 kHz mono WAV through the capture callback as if it were the microphone.
esp_err_t audio_capture_inject_wav(const char *path, bool realtime);
// What the capture callback has received since the last audio_capture_start(): duration, peak and RMS.
typedef struct { uint32_t ms; int peak; int rms; } audio_capture_stats_t;
void      audio_capture_stats(audio_capture_stats_t *out);

// ---- playback ----
esp_err_t audio_play_pcm(const int16_t *pcm, size_t samples);     // copies into the playback ring
esp_err_t audio_play_marker(int id);                               // on_marker(id) fires when playback reaches this point
void      audio_play_flush(void);                                  // drop everything queued, silence now
uint32_t  audio_play_queued_ms(void);
bool      audio_play_idle(void);                                   // nothing queued and nothing sounding
uint8_t   audio_play_level(void);                                  // 0–255: the last frame pulled, RMS / 32 before the volume (the beak)
typedef void (*audio_marker_cb)(int id, void *user);
void      audio_set_marker_cb(audio_marker_cb cb, void *user);     // from the audio task
// The volume, 0–100: 100 is the loudest, every 10 below it 3 dB quieter (10 is 27 dB down), 0 silence; heard at once.
#define AUDIO_VOLUME_DEFAULT 90
void      audio_set_volume(uint8_t percent);

// ---- self-test: is the microphone delivering, and does it hear the speaker? (pairing page, console) ----
typedef struct {
    uint32_t ambient_ms; int ambient_peak, ambient_rms;   // 0.5 s of quiet first
    uint32_t tone_ms;    int tone_peak, tone_rms;         // then a beep from the speaker, listened to
    uint8_t volume; esp_err_t sfx;
} audio_selftest_t;
esp_err_t audio_selftest(audio_selftest_t *out);           // ESP_ERR_INVALID_STATE while a call holds the microphone

// ---- local sound effects (16 kHz mono PCM / WAV files under content/sfx) ----
esp_err_t audio_sfx_play(const char *name);                        // "beep", "over", "coo", "whistle", "nosignal" …
esp_err_t audio_play_file(const char *path);                       // a 16 kHz mono WAV anywhere (data/voice/…): queued like an sfx
