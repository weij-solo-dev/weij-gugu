// provider_wire_openai.c — OpenAI chat.completions streaming (DashScope compatible-mode, Volcengine Ark).
#include "provider_wire.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

bool wire_openai_build(strbuf_t *out, const char *model, const chat_request_t *req, const char *extra_json, const char *max_field, bool temperature)
{
    strbuf_printf(out, "{\"model\":\"%s\",\"stream\":true,\"stream_options\":{\"include_usage\":true},\"%s\":%u", model,
                  max_field && *max_field ? max_field : "max_tokens", (unsigned)(req->max_tokens ? req->max_tokens : 256u));
    if (temperature && req->temperature > 0) strbuf_printf(out, ",\"temperature\":%.2f", (double)req->temperature);
    strbuf_puts(out, ",\"messages\":[");
    bool first = true;
    if (req->system_count) {                      // one system message, blocks joined (no per-block cache control here)
        strbuf_puts(out, "{\"role\":\"system\",\"content\":\"");
        for (size_t i = 0; i < req->system_count; i++) { if (i) strbuf_puts(out, "\\n\\n"); strbuf_append_json_escaped(out, req->system[i].text ? req->system[i].text : ""); }
        strbuf_puts(out, "\"}");
        first = false;
    }
    for (size_t i = 0; i < req->message_count; i++) {
        const chat_message_t *m = &req->messages[i];
        strbuf_printf(out, "%s{\"role\":\"%s\",\"content\":\"", first ? "" : ",", m->role && !strcmp(m->role, "assistant") ? "assistant" : "user");
        strbuf_append_json_escaped(out, m->text ? m->text : "");
        strbuf_puts(out, "\"}");
        first = false;
    }
    strbuf_puts(out, "]");
    if (extra_json && *extra_json) { strbuf_puts(out, ","); strbuf_puts(out, extra_json); }
    strbuf_puts(out, "}");
    return out->s != NULL;
}

void wire_openai_init(wire_openai_t *w, const chat_events_t *ev, void *user)
{
    memset(w, 0, sizeof *w);
    w->ev = *ev; w->user = user;
}

static void fail(wire_openai_t *w, provider_error_kind_t kind, int http, int api, const char *msg)
{
    if (w->done) return;
    w->done = true;
    provider_error_t e; provider_error_set(&e, kind, http, api, msg);
    if (w->ev.on_error) w->ev.on_error(&e, w->user);
}

void wire_openai_event(wire_openai_t *w, const char *event, const char *data)
{
    (void)event;
    if (w->done || !data || !*data) return;
    if (!strcmp(data, "[DONE]")) {
        w->done = true;
        if (w->ev.on_done) w->ev.on_done(w->stop_reason[0] ? w->stop_reason : "stop", &w->usage, w->user);
        return;
    }
    cJSON *j = cJSON_Parse(data);
    if (!j) return;
    cJSON *err = cJSON_GetObjectItem(j, "error");
    if (err) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
        provider_error_kind_t k = PERR_OTHER;
        if (code && (strstr(code, "inspection") || strstr(code, "content"))) k = PERR_CONTENT_REJECTED;
        fail(w, k, 200, 0, m ? m : "stream error");
        cJSON_Delete(j); return;
    }
    cJSON *usage = cJSON_GetObjectItem(j, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(usage, "prompt_tokens")) && cJSON_IsNumber(v)) w->usage.in_tokens = (uint32_t)v->valuedouble;
        if ((v = cJSON_GetObjectItem(usage, "completion_tokens")) && cJSON_IsNumber(v)) w->usage.out_tokens = (uint32_t)v->valuedouble;
        cJSON *det = cJSON_GetObjectItem(usage, "prompt_tokens_details");
        if ((v = cJSON_GetObjectItem(det, "cached_tokens")) && cJSON_IsNumber(v)) w->usage.cached_tokens = (uint32_t)v->valuedouble;
    }
    cJSON *choices = cJSON_GetObjectItem(j, "choices");
    cJSON *c0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (c0) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(c0, "delta"), "content"));
        if (t && *t && w->ev.on_delta) w->ev.on_delta(t, strlen(t), w->user);
        const char *fr = cJSON_GetStringValue(cJSON_GetObjectItem(c0, "finish_reason"));
        if (fr) snprintf(w->stop_reason, sizeof w->stop_reason, "%s", fr);
    }
    cJSON_Delete(j);
}

void wire_openai_http_error(wire_openai_t *w, int http_status, const char *body)
{
    char msg[96] = "";
    provider_error_kind_t kind = provider_classify_http(http_status);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    if (j) {
        cJSON *e = cJSON_GetObjectItem(j, "error");
        const char *em = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(e, "code"));
        if (em) snprintf(msg, sizeof msg, "%s", em);
        if (code && (strstr(code, "inspection") || strstr(code, "content"))) kind = PERR_CONTENT_REJECTED;
        cJSON_Delete(j);
    }
    fail(w, kind, http_status, 0, msg[0] ? msg : "http error");
}
