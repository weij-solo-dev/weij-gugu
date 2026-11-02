#include "foundation/sse.h"
#include <string.h>

void sse_init(sse_parser_t *p, sse_event_cb cb, void *user)
{
    strbuf_init(&p->line); strbuf_init(&p->data);
    p->event[0] = '\0'; p->cb = cb; p->user = user;
}

static void dispatch(sse_parser_t *p)
{
    if (p->data.len == 0 && p->event[0] == '\0') return;
    if (p->data.len > 0 && p->data.s[p->data.len - 1] == '\n') { p->data.len--; p->data.s[p->data.len] = '\0'; }
    p->cb(p->event, p->data.s ? p->data.s : "", p->user);
    strbuf_clear(&p->data);
    p->event[0] = '\0';
}

static void handle_line(sse_parser_t *p, const char *l, size_t n)
{
    if (n == 0) { dispatch(p); return; }
    if (l[0] == ':') return;   // comment / keep-alive
    const char *colon = memchr(l, ':', n);
    size_t flen = colon ? (size_t)(colon - l) : n;
    const char *v = colon ? colon + 1 : l + n;
    size_t vlen = colon ? n - flen - 1 : 0;
    if (vlen && *v == ' ') { v++; vlen--; }
    if (flen == 5 && !memcmp(l, "event", 5)) {
        size_t c = vlen < sizeof p->event - 1 ? vlen : sizeof p->event - 1;
        memcpy(p->event, v, c); p->event[c] = '\0';
    } else if (flen == 4 && !memcmp(l, "data", 4)) {
        strbuf_append(&p->data, v, vlen);
        strbuf_putc(&p->data, '\n');
    }
    // id: and retry: are ignored
}

void sse_feed(sse_parser_t *p, const char *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = bytes[i];
        if (c == '\n') {
            size_t len = p->line.len;
            if (len && p->line.s[len - 1] == '\r') len--;
            handle_line(p, p->line.s ? p->line.s : "", len);
            strbuf_clear(&p->line);
        } else {
            strbuf_putc(&p->line, c);
        }
    }
}

void sse_finish(sse_parser_t *p)
{
    if (p->line.len) { handle_line(p, p->line.s, p->line.len); strbuf_clear(&p->line); }
    dispatch(p);
}

void sse_free(sse_parser_t *p) { strbuf_free(&p->line); strbuf_free(&p->data); }
