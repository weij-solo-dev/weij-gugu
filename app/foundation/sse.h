// foundation/sse.h — incremental Server-Sent-Events parser. Feed arbitrary byte chunks; get one
// callback per complete event with its `event:` name (may be empty) and the joined `data:` payload.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "foundation/strbuf.h"

typedef void (*sse_event_cb)(const char *event, const char *data, void *user);

typedef struct {
    strbuf_t line;      // current partial line
    strbuf_t data;      // accumulated data: lines of the current event
    char event[32];
    sse_event_cb cb;
    void *user;
} sse_parser_t;

void sse_init(sse_parser_t *p, sse_event_cb cb, void *user);
void sse_feed(sse_parser_t *p, const char *bytes, size_t n);
void sse_finish(sse_parser_t *p);   // flush a trailing event without a final blank line
void sse_free(sse_parser_t *p);
