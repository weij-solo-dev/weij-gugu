// provider_wire.h — private: request builders and stream decoders for the two chat wire formats
// (Anthropic Messages, OpenAI chat.completions). Vendor files pick one; nothing here names a vendor.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "provider/provider.h"
#include "foundation/strbuf.h"

// ---- Anthropic Messages (streaming) ----
// extra_json: raw JSON object members to splice in ("\"output_config\":{\"effort\":\"low\"}"), or NULL.
bool wire_anthropic_build(strbuf_t *out, const char *model, const chat_request_t *req, const char *extra_json);

typedef struct {
    chat_events_t ev;
    void *user;
    provider_usage_t usage;
    char stop_reason[24];
    bool done;                 // on_done or on_error already delivered
    bool text_block;           // current content block is text (not thinking)
} wire_anthropic_t;

void wire_anthropic_init(wire_anthropic_t *w, const chat_events_t *ev, void *user);
// Feed one SSE event (name may be empty: Anthropic-compatible servers always set `type` in the data).
void wire_anthropic_event(wire_anthropic_t *w, const char *event, const char *data);
// A non-200 response body: classify and deliver on_error.
void wire_anthropic_http_error(wire_anthropic_t *w, int http_status, const char *body);

// ---- OpenAI chat.completions (streaming; DashScope compatible-mode, Volcengine Ark) ----
bool wire_openai_build(strbuf_t *out, const char *model, const chat_request_t *req, const char *extra_json);
typedef struct {
    chat_events_t ev; void *user;
    provider_usage_t usage; char stop_reason[24]; bool done;
} wire_openai_t;
void wire_openai_init(wire_openai_t *w, const chat_events_t *ev, void *user);
void wire_openai_event(wire_openai_t *w, const char *event, const char *data);
void wire_openai_http_error(wire_openai_t *w, int http_status, const char *body);
