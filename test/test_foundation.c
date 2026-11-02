#include "minitest.h"
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

void test_foundation(void)
{
    RUN(test_ringbuf_wraps);
    RUN(test_strbuf_json_escape);
    RUN(test_sse_split_chunks);
    RUN(test_hex_b64_roundtrip);
    RUN(test_wav_header_roundtrip);
    RUN(test_atomic_file);
}
