// audio_internal.h — private to the audio module.
#pragma once
#include "audio/audio.h"

esp_err_t audio_common_init(const char *sfx_dir);
size_t    audio_pull(int16_t *out, size_t samples);              // output path pulls frames
void      audio_push_capture(const int16_t *pcm, size_t samples); // input path pushes frames
esp_err_t audio_platform_capture(bool on);                        // start/stop the microphone
uint64_t  audio_written_samples(void);                            // samples handed to playback since boot
uint64_t  audio_played_samples(void);                             // … and pulled by the output since boot
extern const uint32_t audio_out_latency_ms;                       // pulled → heard (the output's DMA queue)
void      audio_song_set_dir(const char *dir);                    // where the songs are (audio_common_init sets it)
