#include "provider_wire.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

static const char *ROLE_OK(const char *r) { return (r && !strcmp(r, "assistant")) ? "assistant" : "user"; }

bool wire_anthropic_build(strbuf_t *out, const char *model, const chat_request_t *req, const char *extra_json)
{
    strbuf_printf(out, "{\"model\":\"%s\",\"max_tokens\":%u,\"stream\":true", model, (unsigned)(req->max_tokens ? req->max_tokens : 256u));
    if (req->temperature > 0) strbuf_printf(out, ",\"temperature\":%.2f", (double)req->temperature);
    if (req->system_count) {
        strbuf_puts(out, ",\"system\":[");
        for (size_t i = 0; i < req->system_count; i++) {
            const chat_block_t *b = &req->system[i];
            strbuf_puts(out, i ? ",{\"type\":\"text\",\"text\":\"" : "{\"type\":\"text\",\"text\":\"");
            strbuf_append_json_escaped(out, b->text ? b->text : "");
            strbuf_puts(out, "\"");
            if (b->cache) strbuf_puts(out, ",\"cache_control\":{\"type\":\"ephemeral\"}");
            strbuf_puts(out, "}");
        }
        strbuf_puts(out, "]");
    }
    strbuf_puts(out, ",\"messages\":[");
    for (size_t i = 0; i < req->message_count; i++) {
        const chat_message_t *m = &req->messages[i];
        strbuf_printf(out, "%s{\"role\":\"%s\",\"content\":\"", i ? "," : "", ROLE_OK(m->role));
        strbuf_append_json_escaped(out, m->text ? m->text : "");
        strbuf_puts(out, "\"}");
    }
    strbuf_puts(out, "]");
    if (extra_json && *extra_json) { strbuf_puts(out, ","); strbuf_puts(out, extra_json); }
    strbuf_puts(out, "}");
    return out->s != NULL;
}

void wire_anthropic_init(wire_anthropic_t *w, const chat_events_t *ev, void *user)
{
    memset(w, 0, sizeof *w);
    w->ev = *ev; w->user = user;
}

static void fail(wire_anthropic_t *w, provider_error_kind_t kind, int http, int api, const char *msg)
{
    if (w->done) return;
    w->done = true;
    provider_error_t e; provider_error_set(&e, kind, http, api, msg);
    if (w->ev.on_error) w->ev.on_error(&e, w->user);
}

static void read_usage(wire_anthropic_t *w, cJSON *usage)
{
    if (!cJSON_IsObject(usage)) return;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(usage, "input_tokens")) && cJSON_IsNumber(v)) w->usage.in_tokens = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(usage, "output_tokens")) && cJSON_IsNumber(v)) w->usage.out_tokens = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(usage, "cache_read_input_tokens")) && cJSON_IsNumber(v)) w->usage.cached_tokens = (uint32_t)v->valuedouble;
}

void wire_anthropic_event(wire_anthropic_t *w, const char *event, const char *data)
{
    (void)event;
    if (w->done || !data || !*data) return;
    cJSON *j = cJSON_Parse(data);
    if (!j) return;                                   // keep-alives and partial garbage are ignored
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "type"));
    if (!type) { cJSON_Delete(j); return; }
    if (!strcmp(type, "content_block_start")) {
        cJSON *cb = cJSON_GetObjectItem(j, "content_block");
        const char *ct = cJSON_GetStringValue(cJSON_GetObjectItem(cb, "type"));
        w->text_block = ct && !strcmp(ct, "text");
    } else if (!strcmp(type, "content_block_delta")) {
        cJSON *d = cJSON_GetObjectItem(j, "delta");
        const char *dt = cJSON_GetStringValue(cJSON_GetObjectItem(d, "type"));
        if (dt && !strcmp(dt, "text_delta")) {
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(d, "text"));
            if (t && *t && w->ev.on_delta) w->ev.on_delta(t, strlen(t), w->user);
        }
        // thinking_delta / signature_delta are not voiced
    } else if (!strcmp(type, "message_start")) {
        read_usage(w, cJSON_GetObjectItem(cJSON_GetObjectItem(j, "message"), "usage"));
    } else if (!strcmp(type, "message_delta")) {
        read_usage(w, cJSON_GetObjectItem(j, "usage"));
        const char *sr = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "delta"), "stop_reason"));
        if (sr) snprintf(w->stop_reason, sizeof w->stop_reason, "%s", sr);
    } else if (!strcmp(type, "message_stop")) {
        w->done = true;
        if (w->ev.on_done) w->ev.on_done(w->stop_reason[0] ? w->stop_reason : "end_turn", &w->usage, w->user);
    } else if (!strcmp(type, "error")) {
        cJSON *e = cJSON_GetObjectItem(j, "error");
        const char *et = cJSON_GetStringValue(cJSON_GetObjectItem(e, "type"));
        const char *em = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
        provider_error_kind_t k = PERR_OTHER;
        if (et && strstr(et, "authentication")) k = PERR_AUTH;
        else if (et && (strstr(et, "rate_limit") || strstr(et, "overloaded"))) k = PERR_QUOTA;
        else if (em && (strstr(em, "sensitive") || strstr(em, "content"))) k = PERR_CONTENT_REJECTED;
        fail(w, k, 200, 0, em ? em : (et ? et : "stream error"));
    }
    cJSON_Delete(j);
}

void wire_anthropic_http_error(wire_anthropic_t *w, int http_status, const char *body)
{
    char msg[96] = "";
    int api = 0;
    provider_error_kind_t kind = provider_classify_http(http_status);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    if (j) {
        cJSON *e = cJSON_GetObjectItem(j, "error");
        const char *em = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
        if (em) {
            snprintf(msg, sizeof msg, "%s", em);
            const char *paren = strrchr(em, '(');     // MiniMax appends "(2013)"
            if (paren) api = atoi(paren + 1);
            if (strstr(em, "sensitive") || api == 1026 || api == 1027) kind = PERR_CONTENT_REJECTED;
        }
        cJSON_Delete(j);
    }
    fail(w, kind, http_status, api, msg[0] ? msg : "http error");
}
