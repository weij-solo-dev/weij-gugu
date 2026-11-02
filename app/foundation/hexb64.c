#include "foundation/hexb64.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_cap)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < hex_len && o < out_cap; i += 2) {
        int h = hexval(hex[i]), l = hexval(hex[i + 1]);
        if (h < 0 || l < 0) break;
        out[o++] = (uint8_t)((h << 4) | l);
    }
    return o;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encoded_len(size_t n) { return ((n + 2) / 3) * 4 + 1; }

size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t out_cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        if (o + 5 > out_cap) break;
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    if (o < out_cap) out[o] = '\0';
    return o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

size_t b64_decode(const char *in, size_t n, uint8_t *out, size_t out_cap)
{
    size_t o = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64val(in[i]);
        if (v < 0) { if (in[i] == '=') break; continue; }
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < out_cap) out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return o;
}
