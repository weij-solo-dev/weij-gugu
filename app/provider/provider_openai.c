// provider_openai.c — OpenAI: ASR by upload at release (/v1/audio/transcriptions), CHAT over the chat.completions
// stream (reasoning off, max_completion_tokens), TTS over the speech endpoint (raw 24 kHz PCM streamed per segment,
// turned into 16 kHz by the shared worker), IMAGE by /v1/images/generations (JPEG, base64 in the reply). Models,
// voices and the per-language instruction are in content/providers/openai.json. Written against
// developers.openai.com (2026-11-04); not yet exercised live. The service is not offered in mainland China, Hong Kong
// or Macao (HTTP 403 there).
#define LOG_TAG "openai"
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
#include "foundation/atomic_file.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

#define VENDOR "openai"
#define ASR_MAX_SECONDS 60
static const provider_preset_t *preset(void) { return provider_preset_get(VENDOR); }
static const char *api_key(void) { return provider_credential(VENDOR, "api_key"); }
static int lang_index(lang_t l) { return l == LANG_EN ? 0 : l == LANG_ZH ? 1 : l == LANG_YUE ? 2 : -1; }

// {"error":{"message","code","type"}} → our error; a 403 with no text is the region (the service does not reach there)
static void error_from_body(provider_error_t *e, int status, const char *body, const char *fallback)
{
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    cJSON *err = cJSON_GetObjectItem(j, "error");
    const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message")), *code = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
    provider_error_kind_t k = provider_classify_http(status);
    if (code && (!strcmp(code, "moderation_blocked") || strstr(code, "content"))) k = PERR_CONTENT_REJECTED;
    provider_error_set(e, k, status, 0, m ? m : status == 403 ? "country, region or territory not supported" : fallback);
    cJSON_Delete(j);
}

// ------------------------------------------------------------------- ASR ----
struct asr_session { asr_events_t ev; void *user; asr_params_t p; int16_t *pcm; size_t samples, cap; net_http_t *http; };

static esp_err_t oa_asr_open(const asr_params_t *p, const asr_events_t *ev, void *user, asr_session_t **out)
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

static esp_err_t oa_asr_feed(asr_session_t *s, const int16_t *pcm, size_t n)
{
    if (s->samples + n > s->cap) n = s->cap - s->samples;
    memcpy(s->pcm + s->samples, pcm, n * sizeof(int16_t));
    s->samples += n;
    return ESP_OK;
}

static esp_err_t oa_asr_finish(asr_session_t *s)
{
    static const char BOUNDARY[] = "----gugu4e1b7d";
    strbuf_t body; strbuf_init(&body);
    strbuf_printf(&body, "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n", BOUNDARY, preset()->asr_model);
    strbuf_printf(&body, "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\nContent-Type: audio/wav\r\n\r\n", BOUNDARY);
    uint8_t hdr[WAV_HEADER_BYTES];
    size_t samples = s->samples, pcm_bytes = samples * sizeof(int16_t);
    wav_write_header(hdr, s->p.sample_rate, 1, (uint32_t)pcm_bytes);
    strbuf_append(&body, (const char *)hdr, sizeof hdr);
    strbuf_append(&body, (const char *)s->pcm, pcm_bytes);
    strbuf_printf(&body, "\r\n--%s--\r\n", BOUNDARY);
    s->samples = 0;                                            // a new press speaks into a clean buffer (see provider_minimax.c)
    char ctype[80], bearer[300];
    snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", BOUNDARY);
    snprintf(bearer, sizeof bearer, "Bearer %s", api_key() ? api_key() : "");
    net_header_t h[2] = { { "Authorization", bearer }, { "Content-Type", ctype } };
    net_http_request_t req = { .method = "POST", .url = preset()->asr_url, .headers = h, .header_count = 2,
                               .body = (const uint8_t *)body.s, .body_len = body.len, .timeout_ms = 15000 };
    net_http_response_t resp;
    int64_t t0 = clock_us();
    esp_err_t err = net_http_perform(s->http, &req, &resp);
    strbuf_free(&body);
    LOGI("asr upload %u ms audio: http %d in %lld ms", (unsigned)(samples * 1000 / s->p.sample_rate), resp.status, (long long)((clock_us() - t0) / 1000));
    provider_error_t e;
    if (err != ESP_OK || resp.status != 200) {
        error_from_body(&e, resp.status, resp.body, "transcription failed");
        if (err == ESP_ERR_TIMEOUT) e.kind = PERR_TIMEOUT;
        mem_release(resp.body);
        s->ev.on_error(&e, s->user);
        return ESP_FAIL;
    }
    char text[1024]; lang_t l;
    if (!provider_openai_transcript(resp.body, text, sizeof text, &l)) { provider_error_set(&e, PERR_PROTOCOL, 200, 0, "no text in the transcription"); s->ev.on_error(&e, s->user); }
    else { provider_usage_t u = { .audio_ms = (uint32_t)(samples * 1000 / s->p.sample_rate) }; s->ev.on_final(text, l, &u, s->user); }
    mem_release(resp.body);
    return ESP_OK;
}

bool provider_openai_transcript(const char *json, char *text, size_t cap, lang_t *lang)
{
    cJSON *j = json ? cJSON_Parse(json) : NULL;
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
    if (t) {
        snprintf(text, cap, "%s", t);
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "languages"), 0), "code"));   // [] when unsure
        lang_t l = code ? lang_from_code(code) : LANG_UNKNOWN;
        *lang = l != LANG_UNKNOWN ? l : provider_guess_lang(text);
    }
    cJSON_Delete(j);
    return t != NULL;
}

static void oa_asr_close(asr_session_t *s) { if (!s) return; net_http_close(s->http); mem_release(s->pcm); free(s); }

// ------------------------------------------------------------------ CHAT ----
struct chat_session { chat_events_t ev; void *user; net_http_t *http; wire_openai_t wire; sse_parser_t sse; atomic_bool cancelled; };
static void chat_sse(const char *event, const char *data, void *user) { struct chat_session *s = user; wire_openai_event(&s->wire, event, data); }
static void chat_body(const uint8_t *chunk, size_t n, void *user) { struct chat_session *s = user; sse_feed(&s->sse, (const char *)chunk, n); }   // 200 only: net buffers an error's body in resp.body

static esp_err_t oa_chat_open(const chat_events_t *ev, void *user, chat_session_t **out)
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

static esp_err_t oa_chat_send(chat_session_t *s, const chat_request_t *req)
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
        else { provider_error_t e; provider_error_set(&e, err == ESP_ERR_TIMEOUT ? PERR_TIMEOUT : PERR_PROTOCOL, resp.status, 0, "stream ended early"); s->wire.done = true; s->ev.on_error(&e, s->user); }
    }
    mem_release(resp.body); sse_free(&s->sse); strbuf_free(&body);
    return ESP_OK;
}
static void oa_chat_cancel(chat_session_t *s) { atomic_store(&s->cancelled, true); net_http_cancel(s->http); }
static void oa_chat_close(chat_session_t *s) { if (!s) return; net_http_close(s->http); free(s); }

// ------------------------------------------------------------------- TTS ----
// One POST per segment, the body streaming raw PCM (24 kHz, 16-bit LE) as it is made; the shared worker queues the
// segments and turns the audio into 16 kHz.
struct tts_session { http_tts_t *h; net_http_t *http; };

static void speech_body(const uint8_t *chunk, size_t n, void *user)   // 200 only: an error's JSON lands in resp.body
{
    struct tts_session *s = user;
    http_tts_pcm(s->h, chunk, n, preset()->tts_rate ? preset()->tts_rate : 24000);
}

static esp_err_t oa_speak(http_tts_t *h, const char *text, provider_error_t *err)
{
    struct tts_session *s = http_tts_vendor(h);
    const provider_preset_t *pr = preset();
    const tts_params_t *p = http_tts_params(h);
    const char *voice = provider_voice(pr, p->lang, p->voice_id);
    int li = lang_index(p->lang);
    strbuf_t b; strbuf_init(&b);
    strbuf_printf(&b, "{\"model\":\"%s\",\"voice\":", pr->tts_model);
    if (!strncmp(voice, "voice_", 6)) { strbuf_puts(&b, "{\"id\":\""); strbuf_append_json_escaped(&b, voice); strbuf_puts(&b, "\"}"); }   // a custom voice
    else { strbuf_putc(&b, '"'); strbuf_append_json_escaped(&b, voice); strbuf_putc(&b, '"'); }
    strbuf_puts(&b, ",\"input\":\""); strbuf_append_json_escaped(&b, text); strbuf_puts(&b, "\",\"response_format\":\"pcm\"");
    if (li >= 0 && pr->lang_boost[li][0]) { strbuf_puts(&b, ",\"instructions\":\""); strbuf_append_json_escaped(&b, pr->lang_boost[li]); strbuf_putc(&b, '"'); }
    if (p->speed > 0 && (p->speed < 0.99f || p->speed > 1.01f)) strbuf_printf(&b, ",\"speed\":%.2f", (double)p->speed);
    strbuf_putc(&b, '}');
    char bearer[300]; snprintf(bearer, sizeof bearer, "Bearer %s", api_key() ? api_key() : "");
    net_header_t hd[2] = { { "Authorization", bearer }, { "Content-Type", "application/json" } };
    net_http_request_t r = { .method = "POST", .url = pr->tts_url, .headers = hd, .header_count = 2,
                             .body = (const uint8_t *)b.s, .body_len = b.len, .timeout_ms = 15000, .on_body = speech_body, .user = s };
    net_http_response_t resp;
    esp_err_t e = net_http_perform(s->http, &r, &resp);
    strbuf_free(&b);
    esp_err_t r2 = ESP_OK;
    if (http_tts_stale(h)) r2 = ESP_OK;                        // cancelled: nobody is waiting for this one
    else if (e != ESP_OK || resp.status != 200) {
        error_from_body(err, resp.status, resp.body, e == ESP_ERR_TIMEOUT ? "the voice timed out" : "speech request failed");
        if (e == ESP_ERR_TIMEOUT) err->kind = PERR_TIMEOUT;
        r2 = ESP_FAIL;
    }
    mem_release(resp.body);
    return r2;
}
static void oa_tts_abort(void *vendor) { struct tts_session *s = vendor; net_http_cancel(s->http); }

static esp_err_t oa_tts_open(const tts_params_t *p, const tts_events_t *ev, void *user, tts_session_t **out)
{
    struct tts_session *s = calloc(1, sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    esp_err_t err = net_http_open(preset()->tts_url, &s->http);
    if (err != ESP_OK) { free(s); return err; }
    net_http_prewarm(s->http, 6000);
    s->h = http_tts_create(p, ev, user, oa_speak, oa_tts_abort, s);
    if (!s->h) { net_http_close(s->http); free(s); return ESP_ERR_NO_MEM; }
    *out = s;
    return ESP_OK;
}
static esp_err_t oa_tts_text(tts_session_t *s, const char *utf8, size_t n) { return http_tts_text(s->h, utf8, n); }
static esp_err_t oa_tts_flush(tts_session_t *s) { return http_tts_flush(s->h); }
static esp_err_t oa_tts_cancel(tts_session_t *s) { return http_tts_cancel(s->h); }
static void oa_tts_close(tts_session_t *s) { if (!s) return; http_tts_destroy(s->h); net_http_close(s->http); free(s); }

// ----------------------------------------------------------------- IMAGE ----
static esp_err_t oa_image_generate(const image_params_t *p, provider_usage_t *usage, provider_error_t *err)
{
    const provider_preset_t *pr = preset();
    if (!pr->image_url[0]) { provider_error_set(err, PERR_OTHER, 0, 0, "no image endpoint in the preset"); return ESP_ERR_NOT_SUPPORTED; }
    const char *size = p->aspect && !strcmp(p->aspect, "16:9") ? "1536x1024" : p->aspect && !strcmp(p->aspect, "9:16") ? "1024x1536" : "1024x1024";
    strbuf_t body; strbuf_init(&body);
    strbuf_printf(&body, "{\"model\":\"%s\",\"prompt\":\"", pr->image_model);
    strbuf_append_json_escaped(&body, p->prompt);
    strbuf_printf(&body, "\",\"size\":\"%s\",\"quality\":\"low\",\"output_format\":\"jpeg\",\"output_compression\":85,\"background\":\"opaque\",\"n\":1}", size);
    char bearer[300]; snprintf(bearer, sizeof bearer, "Bearer %s", api_key() ? api_key() : "");
    net_header_t h[2] = { { "Authorization", bearer }, { "Content-Type", "application/json" } };
    net_http_t *http = NULL;
    esp_err_t e = net_http_open(pr->image_url, &http);
    if (e != ESP_OK) { strbuf_free(&body); provider_error_set(err, PERR_NETWORK, 0, 0, "image: open failed"); return e; }
    net_http_request_t req = { .method = "POST", .url = pr->image_url, .headers = h, .header_count = 2, .body = (const uint8_t *)body.s, .body_len = body.len, .timeout_ms = 150000 };
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
    size_t n = 0; uint8_t *jpg = provider_json_b64(resp.body, NULL, "b64_json", &n);
    mem_release(resp.body);
    if (!jpg || n < 1000) { mem_release(jpg); provider_error_set(err, PERR_CONTENT_REJECTED, 200, 0, "no picture came back"); return ESP_FAIL; }
    bool ok = atomic_file_write(p->out_path, jpg, n);
    LOGI("image: %u KB in %lld ms → %s", (unsigned)(n / 1024), (long long)((clock_us() - t0) / 1000), p->out_path);
    mem_release(jpg);
    if (usage) usage->images = 1;
    if (!ok) { provider_error_set(err, PERR_OTHER, 0, 0, "image: could not write the file"); return ESP_FAIL; }
    return ESP_OK;
}

const provider_vtable_t provider_openai = {
    .id = VENDOR,
    .roles = PROVIDER_ROLE_ASR | PROVIDER_ROLE_CHAT | PROVIDER_ROLE_TTS | PROVIDER_ROLE_IMAGE,
    .asr_open = oa_asr_open, .asr_feed = oa_asr_feed, .asr_finish = oa_asr_finish, .asr_close = oa_asr_close,
    .chat_open = oa_chat_open, .chat_send = oa_chat_send, .chat_cancel = oa_chat_cancel, .chat_close = oa_chat_close,
    .tts_open = oa_tts_open, .tts_text = oa_tts_text, .tts_flush = oa_tts_flush, .tts_cancel = oa_tts_cancel, .tts_close = oa_tts_close,
    .image_generate = oa_image_generate,
};
