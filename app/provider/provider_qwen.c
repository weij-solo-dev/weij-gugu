// provider_qwen.c — Alibaba Bailian (DashScope): CHAT over the OpenAI-compatible stream, TTS over the
// CosyVoice duplex WebSocket (run-task / continue-task / finish-task; audio arrives as binary frames).
// The streaming ASR and the speech-to-speech model live in provider_qwen_realtime.c.
// Written against the public protocol docs (2026-11-02); not yet exercised live (no Bailian key).
#define LOG_TAG "qwen"
#include "provider/provider.h"
#include "provider_internal.h"
#include "provider_wire.h"
#include "net/net.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/sse.h"
#include "foundation/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

#define VENDOR "qwen"
static const provider_preset_t *preset(void) { return provider_preset_get(VENDOR); }
static const char *api_key(void) { return provider_credential(VENDOR, "api_key"); }

// ------------------------------------------------------------------ CHAT ----
struct chat_session { chat_events_t ev; void *user; net_http_t *http; wire_openai_t wire; sse_parser_t sse; atomic_bool cancelled; };
static void chat_sse(const char *event, const char *data, void *user) { struct chat_session *s = user; wire_openai_event(&s->wire, event, data); }
static void chat_body(const uint8_t *chunk, size_t n, void *user) { struct chat_session *s = user; sse_feed(&s->sse, (const char *)chunk, n); }

static esp_err_t q_chat_open(const chat_events_t *ev, void *user, chat_session_t **out)
{
    struct chat_session *s = calloc(1, sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    s->ev = *ev; s->user = user;
    esp_err_t err = net_http_open(preset()->chat_url, &s->http);
    if (err != ESP_OK) { free(s); return err; }
    net_http_prewarm(s->http, 6000);
    *out = s;
    return ESP_OK;
}

static esp_err_t q_chat_send(chat_session_t *s, const chat_request_t *req)
{
    const provider_preset_t *p = preset();
    strbuf_t body; strbuf_init(&body);
    int eff = req->effort < 3 ? req->effort : 0;
    wire_openai_build(&body, p->chat_model, req, p->chat_extra[eff][0] ? p->chat_extra[eff] : NULL);
    wire_openai_init(&s->wire, &s->ev, s->user);
    sse_init(&s->sse, chat_sse, s);
    atomic_store(&s->cancelled, false);
    char bearer[300]; snprintf(bearer, sizeof bearer, "Bearer %s", api_key() ? api_key() : "");
    net_header_t h[2] = { { "Authorization", bearer }, { "Content-Type", "application/json" } };
    net_http_request_t r = { .method = "POST", .url = p->chat_url, .headers = h, .header_count = 2,
                             .body = (const uint8_t *)body.s, .body_len = body.len, .timeout_ms = 12000, .on_body = chat_body, .user = s };
    net_http_response_t resp;
    esp_err_t err = net_http_perform(s->http, &r, &resp);
    sse_finish(&s->sse);
    if (!s->wire.done) {
        if (atomic_load(&s->cancelled)) { provider_error_t e; provider_error_set(&e, PERR_CANCELLED, resp.status, 0, "cancelled"); s->wire.done = true; s->ev.on_error(&e, s->user); }
        else if (resp.status != 200) wire_openai_http_error(&s->wire, resp.status, resp.body);
        else { provider_error_t e; provider_error_set(&e, err == ESP_ERR_TIMEOUT ? PERR_TIMEOUT : PERR_PROTOCOL, resp.status, 0, "stream ended early"); s->wire.done = true; s->ev.on_error(&e, s->user); }
    }
    mem_release(resp.body); sse_free(&s->sse); strbuf_free(&body);
    return ESP_OK;
}
static void q_chat_cancel(chat_session_t *s) { atomic_store(&s->cancelled, true); net_http_cancel(s->http); }
static void q_chat_close(chat_session_t *s) { if (!s) return; net_http_close(s->http); free(s); }

// ------------------------------------------------------------------- TTS ----
// One duplex task per segment: run-task → continue-task(text) → finish-task; `task-finished` marks the
// segment complete (the equivalent of MiniMax's flushed). The connection is reused across tasks.
struct tts_session {
    tts_events_t ev; void *user; tts_params_t p;
    net_ws_t *ws;
    char task_id[40];
    strbuf_t pending;              // text of the segment being collected
    atomic_bool task_open, failed;
    signal_t started, finished;
    uint32_t chars_seg;
};

static void new_task_id(char *out)
{
    uint32_t r = (uint32_t)clock_us() ^ (uint32_t)rand();
    snprintf(out, 40, "%08x%08x%08x", (unsigned)r, (unsigned)rand(), (unsigned)rand());
}

static const char *json_str(const char *msg, const char *key, size_t *len)
{
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(msg, pat); if (!p) return NULL;
    p += strlen(pat); const char *e = strchr(p, '"'); if (!e) return NULL;
    *len = (size_t)(e - p); return p;
}

static void tts_text_ev(const char *msg, size_t n, void *user)
{
    struct tts_session *s = user; (void)n;
    size_t el; const char *ev = json_str(msg, "event", &el);
    if (!ev) return;
    if (!strncmp(ev, "task-started", el)) { signal_set(s->started); return; }
    if (!strncmp(ev, "task-finished", el)) {
        atomic_store(&s->task_open, false);
        provider_usage_t u = { .chars = s->chars_seg }; s->chars_seg = 0;
        if (s->ev.on_sentence_end) s->ev.on_sentence_end(s->user);
        if (s->ev.on_flushed) s->ev.on_flushed(&u, s->user);
        signal_set(s->finished);
        return;
    }
    if (!strncmp(ev, "task-failed", el)) {
        size_t ml; const char *m = json_str(msg, "error_message", &ml);
        char buf[96]; snprintf(buf, sizeof buf, "%.*s", (int)(m ? (ml < 90 ? ml : 90) : 0), m ? m : "");
        atomic_store(&s->failed, true); atomic_store(&s->task_open, false);
        provider_error_t e; provider_error_set(&e, strstr(buf, "voice") ? PERR_VOICE_NOT_FOUND : PERR_OTHER, 200, 0, buf[0] ? buf : "task failed");
        s->ev.on_error(&e, s->user);
        signal_set(s->started); signal_set(s->finished);
    }
}
static void tts_bin_ev(const uint8_t *data, size_t n, void *user)
{
    struct tts_session *s = user;
    if (n >= 2) s->ev.on_audio((const int16_t *)data, n / 2, s->user);   // pcm 16k mono per the task parameters
}
static void tts_err_ev(const char *what, void *user)
{
    struct tts_session *s = user;
    if (atomic_exchange(&s->failed, true)) return;
    provider_error_t e; provider_error_set(&e, PERR_NETWORK, 0, 0, what); s->ev.on_error(&e, s->user);
    signal_set(s->started); signal_set(s->finished);
}
static void tts_closed_ev(int code, const char *reason, void *user) { (void)code; (void)reason; struct tts_session *s = user; signal_set(s->started); signal_set(s->finished); }

static esp_err_t q_tts_open(const tts_params_t *p, const tts_events_t *ev, void *user, tts_session_t **out)
{
    const provider_preset_t *pr = preset();
    struct tts_session *s = calloc(1, sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    s->ev = *ev; s->user = user; s->p = *p; strbuf_init(&s->pending);
    s->started = signal_create(); s->finished = signal_create();
    char bearer[300]; snprintf(bearer, sizeof bearer, "Bearer %s", api_key() ? api_key() : "");
    net_header_t h[1] = { { "Authorization", bearer } };
    net_ws_params_t wp = { .url = pr->tts_url, .headers = h, .header_count = 1, .connect_timeout_ms = 6000, .max_message = 256 * 1024 };
    net_ws_events_t we = { .on_text = tts_text_ev, .on_binary = tts_bin_ev, .on_error = tts_err_ev, .on_closed = tts_closed_ev };
    esp_err_t err = net_ws_open(&wp, &we, s, &s->ws);
    if (err != ESP_OK) { signal_free(s->started); signal_free(s->finished); free(s); return err; }
    LOGI("cosyvoice session connected");
    *out = s;
    return ESP_OK;
}

static esp_err_t q_tts_text(tts_session_t *s, const char *utf8, size_t n)
{
    strbuf_append(&s->pending, utf8, n);
    for (size_t i = 0; i < n; i++) if (((unsigned char)utf8[i] & 0xC0) != 0x80) s->chars_seg++;
    return ESP_OK;
}

static esp_err_t q_tts_flush(tts_session_t *s)
{
    if (s->pending.len == 0) { provider_usage_t u = { 0 }; if (s->ev.on_flushed) s->ev.on_flushed(&u, s->user); return ESP_OK; }
    const provider_preset_t *pr = preset();
    const char *voice = provider_voice(pr, s->p.lang, s->p.voice_id);
    new_task_id(s->task_id);
    strbuf_t b; strbuf_init(&b);
    strbuf_printf(&b, "{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
                      "\"payload\":{\"task_group\":\"audio\",\"task\":\"tts\",\"function\":\"SpeechSynthesizer\",\"model\":\"%s\","
                      "\"parameters\":{\"text_type\":\"PlainText\",\"voice\":\"%s\",\"format\":\"pcm\",\"sample_rate\":%u,\"volume\":50,\"rate\":%.2f,\"pitch\":1},\"input\":{}}}",
                  s->task_id, pr->tts_model, voice, (unsigned)(s->p.sample_rate ? s->p.sample_rate : 16000u), (double)(s->p.speed > 0 ? s->p.speed : 1.0f));
    atomic_store(&s->task_open, true);
    net_ws_send_text(s->ws, b.s, b.len);
    if (!signal_wait(s->started, 6000) || atomic_load(&s->failed)) { strbuf_free(&b); strbuf_clear(&s->pending); return ESP_ERR_TIMEOUT; }
    if (s->ev.on_sentence_start) s->ev.on_sentence_start(s->pending.s, s->user);
    strbuf_clear(&b);
    strbuf_printf(&b, "{\"header\":{\"action\":\"continue-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{\"text\":\"", s->task_id);
    strbuf_append_json_escaped(&b, s->pending.s);
    strbuf_puts(&b, "\"}}}");
    net_ws_send_text(s->ws, b.s, b.len);
    strbuf_clear(&b);
    strbuf_printf(&b, "{\"header\":{\"action\":\"finish-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{}}}", s->task_id);
    net_ws_send_text(s->ws, b.s, b.len);
    strbuf_free(&b);
    strbuf_clear(&s->pending);
    return ESP_OK;
}

static esp_err_t q_tts_cancel(tts_session_t *s) { strbuf_clear(&s->pending); return ESP_OK; }   // DashScope has no cancel; the next task simply follows
static void q_tts_close(tts_session_t *s) { if (!s) return; net_ws_close(s->ws); signal_free(s->started); signal_free(s->finished); strbuf_free(&s->pending); free(s); }

const provider_vtable_t provider_qwen = {
    .id = VENDOR,
    .roles = PROVIDER_ROLE_CHAT | PROVIDER_ROLE_TTS,
    .chat_open = q_chat_open, .chat_send = q_chat_send, .chat_cancel = q_chat_cancel, .chat_close = q_chat_close,
    .tts_open = q_tts_open, .tts_text = q_tts_text, .tts_flush = q_tts_flush, .tts_cancel = q_tts_cancel, .tts_close = q_tts_close,
};
