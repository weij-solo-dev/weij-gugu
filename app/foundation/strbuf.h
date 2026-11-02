// foundation/strbuf.h — a growable string (big allocations go to PSRAM via mem.h). Used for JSON
// bodies, prompt assembly and streamed text. Never holds secrets longer than the request.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef struct {
    char *s;
    size_t len, cap;
} strbuf_t;

void strbuf_init(strbuf_t *b);
void strbuf_free(strbuf_t *b);
void strbuf_clear(strbuf_t *b);
bool strbuf_reserve(strbuf_t *b, size_t extra);
bool strbuf_append(strbuf_t *b, const char *s, size_t n);
bool strbuf_puts(strbuf_t *b, const char *s);
bool strbuf_putc(strbuf_t *b, char c);
bool strbuf_printf(strbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool strbuf_append_json_escaped(strbuf_t *b, const char *s);   // escapes for a JSON string literal (no quotes)
char *strbuf_detach(strbuf_t *b);                                // caller frees with mem_release
