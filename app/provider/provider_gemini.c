// provider_gemini.c — Google Gemini (the Developer API with a key, not Vertex AI): ASR by the transcription model in
// one interaction (the WAV inline, base64), CHAT through Gemini's OpenAI-compatible stream (the shared wire codec),
// TTS through a streamed interaction (16 kHz l16, base64 in each step.delta; the shared worker queues segments), IMAGE
// through an interaction (JPEG). Models, voices and the per-language style are in content/providers/gemini.json.
// Written against aistudio.google.com/docs (2026-11-04); not yet exercised live. The API is not offered in mainland
// China, Hong Kong or Macao. Prompts on the free tier may be used to improve Google's products: a paid key for a child.
#define LOG_TAG "gemini"
#include "provider/provider.h"
#include "provider_internal.h"
#include "provider_wire.h"
#include "net/net.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/sse.h"
#include "foundation/wav.h"
#include "foundation/hexb64.h"
#include "foundation/atomic_file.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

#define VENDOR "gemini"
#define ASR_MAX_SECONDS 60
static const provider_preset_t *preset(void) { return provider_preset_get(VENDOR); }
static const char *api_key(void) { return provider_credential(VENDOR, "api_key"); }
static int lang_index(lang_t l) { return l == LANG_EN ? 0 : l == LANG_ZH ? 1 : l == LANG_YUE ? 2 : -1; }

// {"error":{"code","message"}} (an object, or the first of an array: some Google endpoints wrap it)
static void error_from_body(provider_error_t *e, int status, const char *body, const char *fallback)
{
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    cJSON *o = cJSON_IsArray(j) ? cJSON_GetArrayItem(j, 0) : j;
    cJSON *err = cJSON_GetObjectItem(o, "error");
    const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message")), *code = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
    provider_error_kind_t k = provider_classify_http(status);
    if (code && (!strcmp(code, "safety") || !strcmp(code, "prohibited_content") || !strcmp(code, "image_safety"))) k = PERR_CONTENT_REJECTED;
    if (code && (!strcmp(code, "payment_required") || !strcmp(code, "quota_exceeded"))) k = PERR_QUOTA;
    if ((code && !strcmp(code, "authentication")) || (status == 400 && m && strstr(m, "API key"))) k = PERR_AUTH;   // Google answers a bad key with 400
    provider_error_set(e, k, status, 0, m ? m : fallback);
    cJSON_Delete(j);
}

// The first content block of `type` in an interaction's output steps (steps[].content[]); NULL if none.
static cJSON *output_content(cJSON *root, const char *type)
{
    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    for (cJSON *st = cJSON_IsArray(steps) ? steps->child : NULL; st; st = st->next) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(st, "type"));
        if (t && strcmp(t, "model_output")) continue;            // a user_input step echoed back
        cJSON *content = cJSON_GetObjectItem(st, "content");
        for (cJSON *c = cJSON_IsArray(content) ? content->child : NULL; c; c = c->next) {
            const char *ct = cJSON_GetStringValue(cJSON_GetObjectItem(c, "type"));
            if (ct && !strcmp(ct, type)) return c;
        }
    }
    return NULL;
}

static net_http_request_t post(const char *url, net_header_t *h, const strbuf_t *body, uint32_t timeout_ms)
{
    h[0] = (net_header_t){ "x-goog-api-key", api_key() ? api_key() : "" };
    h[1] = (net_header_t){ "Content-Type", "application/json" };
    return (net_http_request_t){ .method = "POST", .url = url, .headers = h, .header_count = 2, .body = (const uint8_t *)body->s, .body_len = body->len, .timeout_ms = timeout_ms };
}

// ------------------------------------------------------------------- ASR ----
struct asr_session { asr_events_t ev; void *user; asr_params_t p; int16_t *pcm; size_t samples, cap; net_http_t *http; };

static esp_err_t gm_asr_open(const asr_params_t *p, const asr_events_t *ev, void *user, asr_session_t **out)
{
    struct asr_session *s = calloc(1, sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    s->ev = *ev; s->user = user; s->p = *p;
    if (!s->p.sample_rate) s->p.sample_rate = 16000;
    s->cap = (size_t)s->p.sample_rate * ASR_MAX_SECONDS;
    s->pcm = mem_alloc_big(s->cap * sizeof(int16_t));
    if (!s->pcm) { free(s); return ESP_ERR_NO_MEM; }
    net_http_open(preset()->asr_url, &s->http);
    net_http_prewarm(s->http, 6000);
    *out = s;
    return ESP_OK;
}

static esp_err_t gm_asr_feed(asr_session_t *s, const int16_t *pcm, size_t n)
{
    if (s->samples + n > s->cap) n = s->cap - s->samples;
    memcpy(s->pcm + s->samples, pcm, n * sizeof(int16_t));
    s->samples += n;
    return ESP_OK;
}

static esp_err_t gm_asr_finish(asr_session_t *s)
{
    size_t samples = s->samples, wav_n = WAV_HEADER_BYTES + samples * sizeof(int16_t);
    uint8_t *wav = mem_alloc_big(wav_n);
    char *b64 = wav ? mem_alloc_big(b64_encoded_len(wav_n) + 1) : NULL;
    provider_error_t e;
    if (!wav || !b64) { mem_release(wav); provider_error_set(&e, PERR_OTHER, 0, 0, "no memory for the audio"); s->ev.on_error(&e, s->user); return ESP_ERR_NO_MEM; }
    wav_write_header(wav, s->p.sample_rate, 1, (uint32_t)(samples * sizeof(int16_t)));
    memcpy(wav + WAV_HEADER_BYTES, s->pcm, samples * sizeof(int16_t));
    s->samples = 0;                                            // a new press speaks into a clean buffer
    size_t bn = b64_encode(wav, wav_n, b64, b64_encoded_len(wav_n) + 1);
    mem_release(wav);
    strbuf_t body; strbuf_init(&body);
    strbuf_printf(&body, "{\"model\":\"%s\",\"input\":[{\"type\":\"audio\",\"mime_type\":\"audio/wav\",\"data\":\"", preset()->asr_model);
    strbuf_append(&body, b64, bn);
    strbuf_puts(&body, "\"}],\"store\":false}");
    mem_release(b64);
    net_header_t h[2]; net_http_request_t req = post(preset()->asr_url, h, &body, 15000);
    net_http_response_t resp;
    int64_t t0 = clock_us();
    esp_err_t err = net_http_perform(s->http, &req, &resp);
    strbuf_free(&body);
    LOGI("asr %u ms audio: http %d in %lld ms", (unsigned)(samples * 1000 / s->p.sample_rate), resp.status, (long long)((clock_us() - t0) / 1000));
    if (err != ESP_OK || resp.status != 200) {
        error_from_body(&e, resp.status, resp.body, "transcription failed");
        if (err == ESP_ERR_TIMEOUT) e.kind = PERR_TIMEOUT;
        mem_release(resp.body); s->ev.on_error(&e, s->user); return ESP_FAIL;
    }
    char text[1024];
    if (!provider_gemini_text(resp.body, text, sizeof text)) { provider_error_set(&e, PERR_PROTOCOL, 200, 0, "no transcript in the reply"); s->ev.on_error(&e, s->user); }
    else { provider_usage_t u = { .audio_ms = (uint32_t)(samples * 1000 / s->p.sample_rate) }; s->ev.on_final(text, provider_guess_lang(text), &u, s->user); }
    mem_release(resp.body);
    return ESP_OK;
}

bool provider_gemini_text(const char *json, char *out, size_t cap)
{
    cJSON *j = json ? cJSON_Parse(json) : NULL;
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(output_content(j, "text"), "text"));
    if (!t && j && cJSON_IsArray(cJSON_GetObjectItem(j, "steps")) && !output_content(j, "text")) t = "";   // a reply with no text: nothing was heard
    if (t) snprintf(out, cap, "%s", t);
    cJSON_Delete(j);
    return t != NULL;
}

uint8_t *provider_gemini_image(const char *json, size_t *n)
{
    *n = 0;
    cJSON *j = json ? cJSON_Parse(json) : NULL;
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(output_content(j, "image"), "data"));
    size_t len = b64 ? strlen(b64) : 0;
    uint8_t *out = len ? mem_alloc_big(len / 4 * 3 + 3) : NULL;
    if (out) *n = b64_decode(b64, len, out, len / 4 * 3 + 3);
    cJSON_Delete(j);
    if (out && !*n) { mem_release(out); out = NULL; }
    return out;
}

size_t provider_gemini_tts_event(const char *event, const char *data, uint8_t *out, size_t cap, uint32_t *rate, char *err, size_t err_cap)
{
    err[0] = '\0';
    if (!data || !*data || !strcmp(data, "[DONE]")) return 0;
    cJSON *j = cJSON_Parse(data);
    if (!j) return 0;
    size_t n = 0;
    cJSON *e = cJSON_GetObjectItem(j, "error");
    if (e || (event && !strcmp(event, "error"))) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
        snprintf(err, err_cap, "%s", m ? m : "voice stream error");
    } else {
        cJSON *d = cJSON_GetObjectItem(j, "delta");
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(d, "type")), *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(d, "data"));
        if (type && !strcmp(type, "audio") && b64) {
            n = b64_decode(b64, strlen(b64), out, cap);
            cJSON *r = cJSON_GetObjectItem(d, "sample_rate");
            *rate = cJSON_IsNumber(r) ? (uint32_t)r->valuedouble : 16000;
        }
    }
    cJSON_Delete(j);
    return n;
}

static void gm_asr_close(asr_session_t *s) { if (!s) return; net_http_close(s->http); mem_release(s->pcm); free(s); }

// ------------------------------------------------------------------ CHAT ----
// Gemini's OpenAI-compatible chat.completions stream: the shared wire codec, a Bearer key.
struct chat_session { chat_events_t ev; void *user; net_http_t *http; wire_openai_t wire; sse_parser_t sse; atomic_bool cancelled; };
static void chat_sse(const char *event, const char *data, void *user) { struct chat_session *s = user; wire_openai_event(&s->wire, event, data); }
static void chat_body(const uint8_t *chunk, size_t n, void *user) { struct chat_session *s = user; sse_feed(&s->sse, (const char *)chunk, n); }   // 200 only: net buffers an error's body in resp.body

static esp_err_t gm_chat_open(const chat_events_t *ev, void *user, chat_session_t **out)
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

static esp_err_t gm_chat_send(chat_session_t *s, const chat_request_t *req)
{
    const provider_preset_t *p = preset();
    strbuf_t body; strbuf_init(&body);
    int eff = req->effort < 3 ? req->effort : 0;
    wire_openai_build(&body, p->chat_model, req, p->chat_extra[eff][0] ? p->chat_extra[eff] : NULL, p->chat_max_field, p->chat_temperature);
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
        else if (resp.status != 200) { provider_error_t e; error_from_body(&e, resp.status, resp.body, "chat request failed"); s->wire.done = true; s->ev.on_error(&e, s->user); }
        else if (s->wire.stop_reason[0]) { s->wire.done = true; s->ev.on_done(s->wire.stop_reason, &s->wire.usage, s->user); }   // ended without [DONE]
        else { provider_error_t e; provider_error_set(&e, err == ESP_ERR_TIMEOUT ? PERR_TIMEOUT : PERR_PROTOCOL, resp.status, 0, "stream ended early"); s->wire.done = true; s->ev.on_error(&e, s->user); }
    }
    mem_release(resp.body); sse_free(&s->sse); strbuf_free(&body);
    return ESP_OK;
}
static void gm_chat_cancel(chat_session_t *s) { atomic_store(&s->cancelled, true); net_http_cancel(s->http); }
static void gm_chat_close(chat_session_t *s) { if (!s) return; net_http_close(s->http); free(s); }

// ------------------------------------------------------------------- TTS ----
// One streamed interaction per segment: each step.delta carries base64 PCM (we ask for 16 kHz l16, so nothing to
// convert); an `error` event or a non-200 status fails the segment.
struct tts_session { http_tts_t *h; net_http_t *http; sse_parser_t sse; provider_error_t stream_err; bool failed; };

static void tts_event(const char *event, const char *data, void *user)
{
    struct tts_session *s = user;
    size_t cap = data ? strlen(data) / 4 * 3 + 3 : 0;
    uint8_t *pcm = cap > 3 ? mem_alloc_big(cap) : NULL;
    if (!pcm) return;
    char err[96]; uint32_t rate = 16000;
    size_t n = provider_gemini_tts_event(event, data, pcm, cap, &rate, err, sizeof err);
    if (n) http_tts_pcm(s->h, pcm, n, rate);
    else if (err[0]) { provider_error_set(&s->stream_err, PERR_OTHER, 200, 0, err); s->failed = true; }
    mem_release(pcm);
}

static void tts_body(const uint8_t *chunk, size_t n, void *user) { struct tts_session *s = user; sse_feed(&s->sse, (const char *)chunk, n); }   // 200 only

static esp_err_t gm_speak(http_tts_t *h, const char *text, provider_error_t *err)
{
    struct tts_session *s = http_tts_vendor(h);
    const provider_preset_t *pr = preset();
    const tts_params_t *p = http_tts_params(h);
    const char *voice = provider_voice(pr, p->lang, p->voice_id);
    int li = lang_index(p->lang);
    strbuf_t b; strbuf_init(&b);
    strbuf_printf(&b, "{\"model\":\"%s\",\"input\":[{\"type\":\"user_input\",\"content\":[{\"type\":\"text\",\"text\":\"", pr->tts_model);
    strbuf_append_json_escaped(&b, text);
    strbuf_putc(&b, '"');
    if (li >= 0 && pr->lang_boost[li][0]) { strbuf_puts(&b, ",\"annotations\":[{\"type\":\"speech_metadata\",\"style\":\""); strbuf_append_json_escaped(&b, pr->lang_boost[li]); strbuf_puts(&b, "\"}]"); }
    strbuf_printf(&b, "}]}],\"response_format\":{\"type\":\"audio\",\"mime_type\":\"audio/l16\",\"sample_rate\":%u},\"generation_config\":{\"speech_config\":[{\"voice\":\"",
                  (unsigned)(p->sample_rate ? p->sample_rate : 16000u));
    strbuf_append_json_escaped(&b, voice);
    strbuf_puts(&b, "\"}]},\"stream\":true,\"store\":false}");
    s->failed = false;
    sse_init(&s->sse, tts_event, s);
    net_header_t hd[2]; net_http_request_t r = post(pr->tts_url, hd, &b, 15000);
    r.on_body = tts_body; r.user = s;
    net_http_response_t resp;
    esp_err_t e = net_http_perform(s->http, &r, &resp);
    sse_finish(&s->sse); sse_free(&s->sse);
    strbuf_free(&b);
    esp_err_t r2 = ESP_OK;
    if (http_tts_stale(h)) r2 = ESP_OK;
    else if (e != ESP_OK || resp.status != 200) {
        error_from_body(err, resp.status, resp.body, e == ESP_ERR_TIMEOUT ? "the voice timed out" : "speech request failed");
        if (e == ESP_ERR_TIMEOUT) err->kind = PERR_TIMEOUT;
        r2 = ESP_FAIL;
    } else if (s->failed) { *err = s->stream_err; r2 = ESP_FAIL; }
    mem_release(resp.body);
    return r2;
}
static void gm_tts_abort(void *vendor) { struct tts_session *s = vendor; net_http_cancel(s->http); }

static esp_err_t gm_tts_open(const tts_params_t *p, const tts_events_t *ev, void *user, tts_session_t **out)
{
    struct tts_session *s = calloc(1, sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    esp_err_t err = net_http_open(preset()->tts_url, &s->http);
    if (err != ESP_OK) { free(s); return err; }
    net_http_prewarm(s->http, 6000);
    s->h = http_tts_create(p, ev, user, gm_speak, gm_tts_abort, s);
    if (!s->h) { net_http_close(s->http); free(s); return ESP_ERR_NO_MEM; }
    *out = s;
    return ESP_OK;
}
static esp_err_t gm_tts_text(tts_session_t *s, const char *utf8, size_t n) { return http_tts_text(s->h, utf8, n); }
static esp_err_t gm_tts_flush(tts_session_t *s) { return http_tts_flush(s->h); }
static esp_err_t gm_tts_cancel(tts_session_t *s) { return http_tts_cancel(s->h); }
static void gm_tts_close(tts_session_t *s) { if (!s) return; http_tts_destroy(s->h); net_http_close(s->http); free(s); }

// ----------------------------------------------------------------- IMAGE ----
static esp_err_t gm_image_generate(const image_params_t *p, provider_usage_t *usage, provider_error_t *err)
{
    const provider_preset_t *pr = preset();
    if (!pr->image_url[0]) { provider_error_set(err, PERR_OTHER, 0, 0, "no image endpoint in the preset"); return ESP_ERR_NOT_SUPPORTED; }
    strbuf_t body; strbuf_init(&body);
    strbuf_printf(&body, "{\"model\":\"%s\",\"input\":\"", pr->image_model);
    strbuf_append_json_escaped(&body, p->prompt);
    strbuf_printf(&body, "\",\"response_format\":{\"type\":\"image\",\"mime_type\":\"image/jpeg\",\"aspect_ratio\":\"%s\",\"image_size\":\"1K\"},\"store\":false}", p->aspect ? p->aspect : "1:1");
    net_http_t *http = NULL;
    esp_err_t e = net_http_open(pr->image_url, &http);
    if (e != ESP_OK) { strbuf_free(&body); provider_error_set(err, PERR_NETWORK, 0, 0, "image: open failed"); return e; }
    net_header_t h[2]; net_http_request_t req = post(pr->image_url, h, &body, 150000);
    net_http_response_t resp = { 0 };
    int64_t t0 = clock_us();
    e = net_http_perform(http, &req, &resp);
    strbuf_free(&body);
    net_http_close(http);
    if (e != ESP_OK || resp.status != 200 || !resp.body) {
        error_from_body(err, resp.status, resp.body, "image request failed");
        if (e == ESP_ERR_TIMEOUT) err->kind = PERR_TIMEOUT;
        mem_release(resp.body); return e != ESP_OK ? e : ESP_FAIL;
    }
    size_t n = 0; uint8_t *jpg = provider_gemini_image(resp.body, &n);
    mem_release(resp.body);
    if (!jpg || n < 1000) { mem_release(jpg); provider_error_set(err, PERR_CONTENT_REJECTED, 200, 0, "no picture came back"); return ESP_FAIL; }
    bool ok = atomic_file_write(p->out_path, jpg, n);
    LOGI("image: %u KB in %lld ms → %s", (unsigned)(n / 1024), (long long)((clock_us() - t0) / 1000), p->out_path);
    mem_release(jpg);
    if (usage) usage->images = 1;
    if (!ok) { provider_error_set(err, PERR_OTHER, 0, 0, "image: could not write the file"); return ESP_FAIL; }
    return ESP_OK;
}

const provider_vtable_t provider_gemini = {
    .id = VENDOR,
    .roles = PROVIDER_ROLE_ASR | PROVIDER_ROLE_CHAT | PROVIDER_ROLE_TTS | PROVIDER_ROLE_IMAGE,
    .asr_open = gm_asr_open, .asr_feed = gm_asr_feed, .asr_finish = gm_asr_finish, .asr_close = gm_asr_close,
    .chat_open = gm_chat_open, .chat_send = gm_chat_send, .chat_cancel = gm_chat_cancel, .chat_close = gm_chat_close,
    .tts_open = gm_tts_open, .tts_text = gm_tts_text, .tts_flush = gm_tts_flush, .tts_cancel = gm_tts_cancel, .tts_close = gm_tts_close,
    .image_generate = gm_image_generate,
};
