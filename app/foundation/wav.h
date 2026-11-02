// foundation/wav.h — 16-bit PCM WAV header helpers (upload-type ASR takes a WAV; replay fixtures are WAVs).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define WAV_HEADER_BYTES 44

// Writes a canonical 44-byte header for mono/stereo 16-bit PCM.
void wav_write_header(uint8_t out[WAV_HEADER_BYTES], uint32_t sample_rate, uint16_t channels, uint32_t pcm_bytes);
// Parses a header; returns false if not 16-bit PCM. *data_offset is where samples start.
bool wav_parse_header(const uint8_t *buf, size_t n, uint32_t *sample_rate, uint16_t *channels,
                      uint32_t *pcm_bytes, size_t *data_offset);
