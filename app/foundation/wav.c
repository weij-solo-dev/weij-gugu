#include "foundation/wav.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }

void wav_write_header(uint8_t out[WAV_HEADER_BYTES], uint32_t sample_rate, uint16_t channels, uint32_t pcm_bytes)
{
    memcpy(out, "RIFF", 4); put32(out + 4, 36 + pcm_bytes); memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4); put32(out + 16, 16); put16(out + 20, 1); put16(out + 22, channels);
    put32(out + 24, sample_rate); put32(out + 28, sample_rate * channels * 2);
    put16(out + 32, channels * 2); put16(out + 34, 16);
    memcpy(out + 36, "data", 4); put32(out + 40, pcm_bytes);
}

bool wav_parse_header(const uint8_t *buf, size_t n, uint32_t *sample_rate, uint16_t *channels,
                      uint32_t *pcm_bytes, size_t *data_offset)
{
    if (n < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return false;
    size_t off = 12;
    bool fmt_ok = false;
    while (off + 8 <= n) {
        uint32_t sz = get32(buf + off + 4);
        if (!memcmp(buf + off, "fmt ", 4)) {
            if (off + 8 + 16 > n) return false;
            if (get16(buf + off + 8) != 1 || get16(buf + off + 22) != 16) return false;
            if (channels) *channels = get16(buf + off + 10);
            if (sample_rate) *sample_rate = get32(buf + off + 12);
            fmt_ok = true;
        } else if (!memcmp(buf + off, "data", 4)) {
            if (!fmt_ok) return false;
            if (pcm_bytes) *pcm_bytes = sz;
            if (data_offset) *data_offset = off + 8;
            return true;
        }
        off += 8 + sz + (sz & 1);
    }
    return false;
}
