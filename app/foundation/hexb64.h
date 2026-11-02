// foundation/hexb64.h — hex and base64 codecs (vendors ship audio as hex or base64 inside JSON).
#pragma once
#include <stddef.h>
#include <stdint.h>

size_t hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_cap);   // returns bytes written
size_t b64_encoded_len(size_t n);
size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t out_cap);          // NUL-terminated, returns length
size_t b64_decode(const char *in, size_t n, uint8_t *out, size_t out_cap);
