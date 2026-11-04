#include "minitest.h"
#include "foundation/resample.h"
#include <math.h>
#include "foundation/motion.h"
#include "foundation/logcap.h"
#include "foundation/log.h"
#include <math.h>
#include "foundation/ringbuf.h"
#include "foundation/strbuf.h"
#include "foundation/sse.h"
#include "foundation/hexb64.h"
#include "foundation/wav.h"
#include "foundation/atomic_file.h"
#include "foundation/mem.h"
#include <stdlib.h>

TEST(test_ringbuf_wraps)
{
    uint8_t storage[8]; ringbuf_t r; ringbuf_init(&r, storage, sizeof storage);
    CHECK_EQ_INT(ringbuf_free(&r), 7);
    CHECK_EQ_INT(ringbuf_write(&r, "abcde", 5), 5);
    uint8_t out[8];
    CHECK_EQ_INT(ringbuf_read(&r, out, 3), 3);
    CHECK(!memcmp(out, "abc", 3));
    CHECK_EQ_INT(ringbuf_write(&r, "fghij", 5), 5);      // wraps around the end
    CHECK_EQ_INT(ringbuf_used(&r), 7);
    CHECK_EQ_INT(ringbuf_write(&r, "k", 1), 0);          // full
    CHECK_EQ_INT(ringbuf_read(&r, out, 8), 7);
    CHECK(!memcmp(out, "defghij", 7));
}

TEST(test_strbuf_json_escape)
{
    strbuf_t b; strbuf_init(&b);
    strbuf_puts(&b, "x=");
    strbuf_append_json_escaped(&b, "he said \"hi\"\n\ttab\\");
    CHECK_EQ_STR(b.s, "x=he said \\\"hi\\\"\\n\\ttab\\\\");
    strbuf_printf(&b, " %d%s", 42, "!");
    CHECK_EQ_STR(b.s + b.len - 4, " 42!");
    strbuf_free(&b);
}

static int sse_count; static char sse_last_event[32]; static char sse_last_data[128];
static void on_sse(const char *ev, const char *data, void *u) { sse_count++; snprintf(sse_last_event, 32, "%s", ev); snprintf(sse_last_data, 128, "%s", data); }

TEST(test_sse_split_chunks)
{
    sse_parser_t p; sse_init(&p, on_sse, NULL); sse_count = 0;
    const char *stream = "event: content_block_delta\r\ndata: {\"a\":1}\r\n\r\n: ping\n\ndata: l1\ndata: l2\n\n";
    for (size_t i = 0; stream[i]; i++) sse_feed(&p, stream + i, 1);   // one byte at a time
    CHECK_EQ_INT(sse_count, 2);
    CHECK_EQ_STR(sse_last_event, "");
    CHECK_EQ_STR(sse_last_data, "l1\nl2");
    sse_feed(&p, "data: tail", 10); sse_finish(&p);
    CHECK_EQ_INT(sse_count, 3);
    CHECK_EQ_STR(sse_last_data, "tail");
    sse_free(&p);
}

TEST(test_hex_b64_roundtrip)
{
    uint8_t out[8];
    CHECK_EQ_INT(hex_decode("00ff10Ab", 8, out, 8), 4);
    CHECK(out[0] == 0x00 && out[1] == 0xff && out[2] == 0x10 && out[3] == 0xab);
    char enc[32]; uint8_t dec[16];
    const uint8_t raw[] = { 'g', 'u', 'g', 'u', 0, 255, 7 };
    size_t n = b64_encode(raw, sizeof raw, enc, sizeof enc);
    CHECK_EQ_STR(enc, "Z3VndQD/Bw==");
    CHECK_EQ_INT(b64_decode(enc, n, dec, sizeof dec), sizeof raw);
    CHECK(!memcmp(dec, raw, sizeof raw));
}

TEST(test_wav_header_roundtrip)
{
    uint8_t h[WAV_HEADER_BYTES];
    wav_write_header(h, 16000, 1, 32000);
    uint32_t sr, bytes; uint16_t ch; size_t off;
    CHECK(wav_parse_header(h, sizeof h, &sr, &ch, &bytes, &off));
    CHECK_EQ_INT(sr, 16000); CHECK_EQ_INT(ch, 1); CHECK_EQ_INT(bytes, 32000); CHECK_EQ_INT(off, 44);
}

TEST(test_atomic_file)
{
    const char *path = "/tmp/gugu_test_atomic.json";
    CHECK(atomic_file_write(path, "{\"v\":1}", 7));
    size_t n; char *s = file_read_all(path, &n);
    CHECK_EQ_INT(n, 7); CHECK_EQ_STR(s, "{\"v\":1}"); mem_release(s);
    CHECK(atomic_file_write(path, "{\"v\":22}", 8));
    s = file_read_all(path, &n); CHECK_EQ_STR(s, "{\"v\":22}"); mem_release(s);
    CHECK(file_append(path, "\nx", 2));
    s = file_read_all(path, &n); CHECK_EQ_INT(n, 10); mem_release(s);
    remove(path); remove("/tmp/gugu_test_atomic.json.tmp");
}

TEST(test_motion_detector)
{
    motion_t m; motion_init(&m); int inten = 0; int shakes = 0, pickups = 0;
    // at rest on the table for a second: nothing
    for (int i = 0; i < 50; i++) { motion_event_t e = motion_feed(&m, 0.02f, -0.01f, 1.0f, i * 20, &inten); CHECK(e == MOTION_NONE); }
    // a hard shake: |a| swings 0.3 .. 1.9 g at 5 Hz for a second
    for (int i = 0; i < 50; i++) {
        float t = (50 + i) * 20 / 1000.0f, s = sinf(2 * 3.14159f * 5 * t);
        motion_event_t e = motion_feed(&m, 0.8f * s, 0.1f, 1.0f + 0.8f * s, (50 + i) * 20, &inten);
        if (e == MOTION_SHAKE) shakes++;
        CHECK(e != MOTION_PICKUP);
    }
    printf("  shakes reported in one second of rattling: %d (intensity %d)\n", shakes, inten);
    CHECK(shakes >= 1 && shakes <= 4);        // at most once per ~300 ms while it goes on
    CHECK(inten >= 40);
    // calm again for two seconds, then a tilt: a pick-up, once
    for (int i = 0; i < 100; i++) CHECK(motion_feed(&m, 0.02f, -0.01f, 1.0f, 2000 + i * 20, &inten) == MOTION_NONE);
    for (int i = 0; i < 30; i++) if (motion_feed(&m, 0.6f, 0.1f, 0.75f, 4000 + i * 20, &inten) == MOTION_PICKUP) pickups++;
    CHECK_EQ_INT(pickups, 1);
    // gentle carrying (small wobble) is neither
    for (int i = 0; i < 100; i++) { float w = 0.1f * sinf(i * 0.4f); CHECK(motion_feed(&m, 0.6f + w, 0.1f, 0.75f - w, 6000 + i * 20, &inten) == MOTION_NONE); }
}

TEST(test_logcap_ring)
{
    CHECK(logcap_init(256));
    char buf[600]; while (logcap_take(buf, sizeof buf)) { }      // whatever earlier log lines left behind
    logcap_write("hello\n", 6);
    CHECK_EQ_INT(logcap_pending(), 6);
    size_t n = logcap_take(buf, sizeof buf); buf[n] = 0;
    CHECK_EQ_STR(buf, "hello\n");
    CHECK_EQ_INT(logcap_take(buf, sizeof buf), 0);
    char big[300]; for (int i = 0; i < 300; i++) big[i] = (char)('a' + i % 26);    // more than the ring holds
    logcap_write(big, 300);
    n = logcap_take(buf, sizeof buf); buf[n] = 0;
    CHECK(strstr(buf, "44 bytes of log lost") != NULL);
    CHECK(n >= 256 && memcmp(buf + n - 256, big + 44, 256) == 0);
    logcap_write("0123456789", 10);                              // small takes come out in order
    n = logcap_take(buf, 4); CHECK_EQ_INT(n, 4); CHECK(!memcmp(buf, "0123", 4));
    n = logcap_take(buf, 100); CHECK_EQ_INT(n, 6); CHECK(!memcmp(buf, "456789", 6));
    LOGI("logcap test line %d", 42);                             // the host LOG macros land in the ring, time-stamped
    n = logcap_take(buf, sizeof buf); buf[n] = 0;
    CHECK(strstr(buf, "logcap test line 42") != NULL);
    CHECK(buf[2] == ':' && buf[5] == ':');                       // "HH:MM:SS " in front
}

// The vendors whose voices come at 24 kHz are played at 16 kHz: a 1 kHz tone keeps its pitch and loudness, a tone above
// the new Nyquist (8 kHz) is filtered out instead of folding back as a whistle, and chunks give what one call gives.
static double tone_rms(const int16_t *x, size_t n) { double s = 0; for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i]; return sqrt(s / (double)n); }
static int crossings(const int16_t *x, size_t n) { int c = 0; for (size_t i = 1; i < n; i++) c += (x[i - 1] < 0) != (x[i] < 0); return c; }
TEST(test_resample_24k_to_16k)
{
    enum { N = 24000 };                                    // one second
    static int16_t in[N], out[N], out2[N];
    for (int i = 0; i < N; i++) in[i] = (int16_t)(10000.0 * sin(2 * M_PI * 1000.0 * i / 24000.0));
    resample_3to2_t r; resample_3to2_init(&r);
    size_t n = resample_3to2(&r, in, N, out);
    CHECK(n >= 15990 && n <= 16000);
    int z = crossings(out + 200, n - 400);                 // 1 kHz → 2000 crossings a second
    CHECK(z > 1940 * (int)(n - 400) / 16000 && z < 2060 * (int)(n - 400) / 16000);
    double g = tone_rms(out + 200, n - 400) / tone_rms(in, N);
    CHECK(g > 0.9 && g < 1.1);
    resample_3to2_init(&r);                                 // the same in uneven chunks
    size_t m = 0;
    for (size_t i = 0; i < N; ) { size_t k = (i % 7 == 0) ? 1 : 1001; if (i + k > N) k = N - i; m += resample_3to2(&r, in + i, k, out2 + m); i += k; }
    CHECK_EQ_INT(m, n);
    CHECK(memcmp(out, out2, n * sizeof(int16_t)) == 0);
    for (int i = 0; i < N; i++) in[i] = (int16_t)(10000.0 * sin(2 * M_PI * 10000.0 * i / 24000.0));   // 10 kHz: above 8 kHz
    resample_3to2_init(&r); n = resample_3to2(&r, in, N, out);
    CHECK(tone_rms(out + 200, n - 400) / tone_rms(in, N) < 0.1);
}

void test_foundation(void)
{
    RUN(test_resample_24k_to_16k);
    RUN(test_motion_detector);
    RUN(test_logcap_ring);
    RUN(test_ringbuf_wraps);
    RUN(test_strbuf_json_escape);
    RUN(test_sse_split_chunks);
    RUN(test_hex_b64_roundtrip);
    RUN(test_wav_header_roundtrip);
    RUN(test_atomic_file);
}
