#include "foundation/strbuf.h"
#include "foundation/mem.h"
#include <string.h>
#include <stdio.h>

void strbuf_init(strbuf_t *b) { b->s = NULL; b->len = b->cap = 0; }
void strbuf_free(strbuf_t *b) { mem_release(b->s); strbuf_init(b); }
void strbuf_clear(strbuf_t *b) { b->len = 0; if (b->s) b->s[0] = '\0'; }

bool strbuf_reserve(strbuf_t *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap *= 2;
    char *n = mem_alloc_big(cap);
    if (!n) return false;
    if (b->s) { memcpy(n, b->s, b->len); mem_release(b->s); }
    b->s = n; b->cap = cap; b->s[b->len] = '\0';
    return true;
}

bool strbuf_append(strbuf_t *b, const char *s, size_t n)
{
    if (!strbuf_reserve(b, n)) return false;
    memcpy(b->s + b->len, s, n);
    b->len += n; b->s[b->len] = '\0';
    return true;
}

bool strbuf_puts(strbuf_t *b, const char *s) { return strbuf_append(b, s, strlen(s)); }
bool strbuf_putc(strbuf_t *b, char c) { return strbuf_append(b, &c, 1); }

bool strbuf_printf(strbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !strbuf_reserve(b, (size_t)n)) { va_end(ap2); return false; }
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return true;
}

bool strbuf_append_json_escaped(strbuf_t *b, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bool ok;
        switch (*p) {
        case '"':  ok = strbuf_puts(b, "\\\""); break;
        case '\\': ok = strbuf_puts(b, "\\\\"); break;
        case '\n': ok = strbuf_puts(b, "\\n"); break;
        case '\r': ok = strbuf_puts(b, "\\r"); break;
        case '\t': ok = strbuf_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) ok = strbuf_printf(b, "\\u%04x", *p);
            else ok = strbuf_putc(b, (char)*p);
        }
        if (!ok) return false;
    }
    return true;
}

char *strbuf_detach(strbuf_t *b)
{
    char *s = b->s;
    strbuf_init(b);
    return s;
}
