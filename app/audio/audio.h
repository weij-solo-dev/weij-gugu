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
uint8_t   audio_play_level(void);                                  // 0–255 RMS of the last frame pulled by the output
typedef void (*audio_marker_cb)(int id, void *user);
void      audio_set_marker_cb(audio_marker_cb cb, void *user);     // from the audio task
void      audio_set_volume(uint8_t percent);

// ---- local sound effects (16 kHz mono PCM / WAV files under content/sfx) ----
esp_err_t audio_sfx_play(const char *name);                        // "dial", "coo", "whistle", "nosignal" …
