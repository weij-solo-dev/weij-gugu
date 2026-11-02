// provider_minimax.c — MiniMax: ASR by multipart upload (asr-1.0), CHAT over the Anthropic-compatible
// stream (M3.1-Flash / M3), TTS over the bidirectional WebSocket (speech-2.8-turbo), image-01 later.
// Everything vendor-specific about MiniMax lives here and in content/providers/minimax.json.
#define LOG_TAG "minimax"
#include "provider/provider.h"
#include "provider_internal.h"
#include "provider_wire.h"
#include "net/net.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/sse.h"
#include "foundation/hexb64.h"
#include "foundation/wav.h"
#include "foundation/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

#define VENDOR "minimax"
#define ASR_MAX_SECONDS 20

static const provider_preset_t *preset(void) { return provider_preset_get(VENDOR); }
static const char *api_key(void) { return provider_credential(VENDOR, "api_key"); }

static void auth_headers(net_header_t *h, char *bearer, size_t cap, bool anthropic)   // cap >= 300
{
    snprintf(bearer, cap, "%s%s", anthropic ? "" : "Bearer ", api_key() ? api_key() : "");
    h[0].name = anthropic ? "x-api-key" : "Authorization"; h[0].value = bearer;
    h[1].name = "anthropic-version"; h[1].value = "2023-06-01";
    h[2].name = "content-type"; h[2].value = "application/json";
}

// Heuristic language of a transcript (MiniMax does not return one): Cantonese particles, else CJK, else English.
static lang_t guess_lang(const char *t)
{
    static const char *const yue[] = { "係", "唔", "佢", "嘅", "咗", "啲", "乜", "嘢", "喺", "點", "冇", "睇", NULL };
    for (int i = 0; yue[i]; i++) if (strstr(t, yue[i])) return LANG_YUE;
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) if (*p >= 0xE4 && *p <= 0xE9) return LANG_ZH;
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) return LANG_EN;
    return LANG_UNKNOWN;
}

// ------------------------------------------------------------------- ASR ----
struct asr_session {
    asr_events_t ev; void *user; asr_params_t p;
    int16_t *pcm; size_t samples, cap;
    net_http_t *http;
};

static esp_err_t mm_asr_open(const asr_params_t *p, const asr_events_t *ev, void *user, asr_session_t **out)
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

static esp_err_t mm_asr_feed(asr_session_t *s, const int16_t *pcm, size_t n)
{
    if (s->samples + n > s->cap) n = s->cap - s->samples;
    memcpy(s->pcm + s->samples, pcm, n * sizeof(int16_t));
    s->samples += n;
    return ESP_OK;
}

static esp_err_t mm_asr_finish(asr_session_t *s)
{
    static const char BOUNDARY[] = "----gugu7f3a9c";
    strbuf_t body; strbuf_init(&body);
    strbuf_printf(&body, "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n", BOUNDARY, preset()->asr_model);
    strbuf_printf(&body, "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\nContent-Type: audio/wav\r\n\r\n", BOUNDARY);
    uint8_t hdr[WAV_HEADER_BYTES];
    size_t pcm_bytes = s->samples * sizeof(int16_t);
    wav_write_header(hdr, s->p.sample_rate, 1, (uint32_t)pcm_bytes);
    strbuf_append(&body, (const char *)hdr, sizeof hdr);
    strbuf_append(&body, (const char *)s->pcm, pcm_bytes);
    strbuf_printf(&body, "\r\n--%s--\r\n", BOUNDARY);

    char ctype[80], bearer[160];
    snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", BOUNDARY);
    snprintf(bearer, sizeof bearer, "Bearer %s", api_key() ? api_key() : "");
    net_header_t h[2] = { { "Authorization", bearer }, { "Content-Type", ctype } };
    net_http_request_t req = { .method = "POST", .url = preset()->asr_url, .headers = h, .header_count = 2,
                               .body = (const uint8_t *)body.s, .body_len = body.len, .timeout_ms = 15000 };
    net_http_response_t resp;
    int64_t t0 = clock_us();
    esp_err_t err = net_http_perform(s->http, &req, &resp);
    strbuf_free(&body);
    LOGI("asr upload %u ms audio: http %d in %lld ms (connect %lld, reused %d)", (unsigned)(s->samples * 1000 / s->p.sample_rate),
         resp.status, (long long)((clock_us() - t0) / 1000), (long long)(resp.t_connect_us / 1000), (int)resp.reused_connection);
    provider_error_t e;
    if (err != ESP_OK || resp.status != 200) {
        provider_error_set(&e, err == ESP_ERR_TIMEOUT ? PERR_TIMEOUT : provider_classify_http(resp.status), resp.status, 0,
                           resp.body ? resp.body : "asr request failed");
        mem_release(resp.body);
        s->samples = 0;
        s->ev.on_error(&e, s->user);
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(resp.body);
    const char *text = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "text")) : NULL;
    cJSON *br = j ? cJSON_GetObjectItem(j, "base_resp") : NULL;
    int code = br ? (int)cJSON_GetNumberValue(cJSON_GetObjectItem(br, "status_code")) : 0;
    if (!j || code != 0 || !text) {
        const char *msg = br ? cJSON_GetStringValue(cJSON_GetObjectItem(br, "status_msg")) : "bad asr response";
        provider_error_set(&e, code == 1026 || code == 1027 ? PERR_CONTENT_REJECTED : PERR_PROTOCOL, 200, code, msg);
        s->ev.on_error(&e, s->user);
    } else {
        provider_usage_t u = { .audio_ms = (uint32_t)(s->samples * 1000 / s->p.sample_rate) };
        const char *lang = cJSON_GetStringValue(cJSON_GetObjectItem(j, "language"));
        lang_t l = lang ? lang_from_code(lang) : guess_lang(text);
        s->ev.on_final(text, l, &u, s->user);
    }
    if (j) cJSON_Delete(j);
    mem_release(resp.body);
    s->samples = 0;                 // ready for the next utterance of this call
    return ESP_OK;
}

static void mm_asr_close(asr_session_t *s)
{
    if (!s) return;
    net_http_close(s->http);
    mem_release(s->pcm);
    free(s);
}

// ------------------------------------------------------------------ CHAT ----
struct chat_session {
    chat_events_t ev; void *user;
    net_http_t *http;
    wire_anthropic_t wire;
    sse_parser_t sse;
    atomic_bool cancelled;
};

static void chat_sse_event(const char *event, const char *data, void *user)
{
    struct chat_session *s = user;
    wire_anthropic_event(&s->wire, event, data);
}

static void chat_body(const uint8_t *chunk, size_t n, void *user)
{
    struct chat_session *s = user;
    sse_feed(&s->sse, (const char *)chunk, n);
}

static esp_err_t mm_chat_open(const chat_events_t *ev, void *user, chat_session_t **out)
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

static esp_err_t mm_chat_send(chat_session_t *s, const chat_request_t *req)
{
    const provider_preset_t *p = preset();
    strbuf_t body; strbuf_init(&body);
    int eff = req->effort < 3 ? req->effort : 0;
    wire_anthropic_build(&body, p->chat_model, req, p->chat_extra[eff][0] ? p->chat_extra[eff] : NULL);
    wire_anthropic_init(&s->wire, &s->ev, s->user);
    sse_init(&s->sse, chat_sse_event, s);
    atomic_store(&s->cancelled, false);

    char bearer[300]; net_header_t h[3];
    auth_headers(h, bearer, sizeof bearer, true);
    net_http_request_t r = { .method = "POST", .url = p->chat_url, .headers = h, .header_count = 3,
                             .body = (const uint8_t *)body.s, .body_len = body.len, .timeout_ms = 12000,
                             .on_body = chat_body, .user = s };
    net_http_response_t resp;
    int64_t t0 = clock_us();
    esp_err_t err = net_http_perform(s->http, &r, &resp);
    sse_finish(&s->sse);
    LOGI("chat: http %d, first byte %lld ms, done %lld ms, reused %d, in %u out %u cached %u",
         resp.status, (long long)(resp.t_first_byte_us / 1000), (long long)((clock_us() - t0) / 1000), (int)resp.reused_connection,
         (unsigned)s->wire.usage.in_tokens, (unsigned)s->wire.usage.out_tokens, (unsigned)s->wire.usage.cached_tokens);
    if (!s->wire.done) {
        if (atomic_load(&s->cancelled)) { provider_error_t e; provider_error_set(&e, PERR_CANCELLED, resp.status, 0, "cancelled"); s->wire.done = true; s->ev.on_error(&e, s->user); }
        else if (resp.status != 200) wire_anthropic_http_error(&s->wire, resp.status, resp.body);
        else if (err == ESP_ERR_TIMEOUT) { provider_error_t e; provider_error_set(&e, PERR_TIMEOUT, 200, 0, "stream stalled"); s->wire.done = true; s->ev.on_error(&e, s->user); }
        else { provider_error_t e; provider_error_set(&e, PERR_PROTOCOL, resp.status, 0, "stream ended early"); s->wire.done = true; s->ev.on_error(&e, s->user); }
    }
    mem_release(resp.body);
    sse_free(&s->sse);
    strbuf_free(&body);
    return ESP_OK;
}

static void mm_chat_cancel(chat_session_t *s) { atomic_store(&s->cancelled, true); net_http_cancel(s->http); }
static void mm_chat_close(chat_session_t *s) { if (!s) return; net_http_close(s->http); free(s); }

// ------------------------------------------------------------------- TTS ----
struct tts_session {
    tts_events_t ev; void *user; tts_params_t p;
    net_ws_t *ws;
    signal_t started;          // task_started received
    signal_t flushed;
    atomic_bool failed;
    uint32_t chars_sent;
    uint32_t chars_reported;
    uint32_t chars_billed;
    int16_t pcm[1024];
};

// Find "key":"value" in a JSON text without building a tree (the audio value can be 100 KB of hex).
static const char *json_str(const char *msg, const char *key, size_t *len)
{
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(msg, pat);
    if (!p) return NULL;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return NULL;
    *len = (size_t)(e - p);
    return p;
}

static void tts_on_text(const char *msg, size_t n, void *user)
{
    struct tts_session *s = user;
    size_t el; const char *ev = json_str(msg, "event", &el);
    if (!ev) return;
    if (!strncmp(ev, "task_started", el)) { signal_set(s->started); return; }
    if (!strncmp(ev, "connected_success", el)) return;
    if (!strncmp(ev, "sentence_start", el)) { if (s->ev.on_sentence_start) s->ev.on_sentence_start(NULL, s->user); return; }
    if (!strncmp(ev, "sentence_end", el)) { if (s->ev.on_sentence_end) s->ev.on_sentence_end(s->user); return; }
    if (!strncmp(ev, "task_continued", el)) {
        size_t hl; const char *hex = json_str(msg, "audio", &hl);
        while (hex && hl >= 4) {
            size_t take = hl > sizeof s->pcm * 2 ? sizeof s->pcm * 2 : hl;
            take &= ~(size_t)3;
            size_t bytes = hex_decode(hex, take, (uint8_t *)s->pcm, sizeof s->pcm);
            if (bytes >= 2) s->ev.on_audio(s->pcm, bytes / 2, s->user);
            hex += take; hl -= take;
        }
        size_t ul; const char *uc = json_str(msg, "usage_characters", &ul);
        if (uc) s->chars_billed = (uint32_t)atoi(uc);
        return;
    }
    if (!strncmp(ev, "task_flushed", el)) {
        provider_usage_t u = { .chars = s->chars_sent - s->chars_reported };
        s->chars_reported = s->chars_sent;
        signal_set(s->flushed);
        if (s->ev.on_flushed) s->ev.on_flushed(&u, s->user);
        return;
    }
    if (!strncmp(ev, "task_failed", el) || !strncmp(ev, "task_canceled", el)) {
        size_t ml; const char *m = json_str(msg, "status_msg", &ml);
        char buf[96]; snprintf(buf, sizeof buf, "%.*s", (int)(ml < 90 ? ml : 90), m ? m : ev);
        if (!strncmp(ev, "task_failed", el)) {
            atomic_store(&s->failed, true);
            provider_error_t e; provider_error_set(&e, strstr(buf, "voice") ? PERR_VOICE_NOT_FOUND : PERR_OTHER, 200, 0, buf);
            s->ev.on_error(&e, s->user);
        }
        signal_set(s->started); signal_set(s->flushed);
        return;
    }
    if (!strncmp(ev, "task_finished", el)) return;
    LOGD("tts event %.*s", (int)el, ev);
}

static void tts_on_error(const char *what, void *user)
{
    struct tts_session *s = user;
    if (atomic_exchange(&s->failed, true)) return;
    provider_error_t e; provider_error_set(&e, PERR_NETWORK, 0, 0, what);
    s->ev.on_error(&e, s->user);
    signal_set(s->started); signal_set(s->flushed);
}

static void tts_on_closed(int code, const char *reason, void *user)
{
    struct tts_session *s = user;
    (void)reason;
    LOGI("tts ws closed (%d)", code);
    signal_set(s->started); signal_set(s->flushed);
}

static esp_err_t mm_tts_open(const tts_params_t *p, const tts_events_t *ev, void *user, tts_session_t **out)
{
    const provider_preset_t *pr = preset();
    struct tts_session *s = mem_alloc_big(sizeof *s);   // holds a 2 KB PCM scratch buffer
    if (!s) return ESP_ERR_NO_MEM;
    s->ev = *ev; s->user = user; s->p = *p;
    s->started = signal_create(); s->flushed = signal_create();
    char bearer[300]; snprintf(bearer, sizeof bearer, "Bearer %s", api_key() ? api_key() : "");
    net_header_t h[1] = { { "Authorization", bearer } };
    net_ws_params_t wp = { .url = pr->tts_url, .headers = h, .header_count = 1, .connect_timeout_ms = 6000, .max_message = 512 * 1024 };
    net_ws_events_t we = { .on_text = tts_on_text, .on_error = tts_on_error, .on_closed = tts_on_closed };
    int64_t t0 = clock_us();
    esp_err_t err = net_ws_open(&wp, &we, s, &s->ws);
    if (err != ESP_OK) { mem_release(s); return err; }
    int li = p->lang == LANG_EN ? 0 : p->lang == LANG_ZH ? 1 : p->lang == LANG_YUE ? 2 : 1;
    const char *voice = p->voice_id ? p->voice_id : (pr->default_voice_lang[li][0] ? pr->default_voice_lang[li] : pr->default_voice);
    const char *boost = p->lang == LANG_UNKNOWN ? "auto" : (pr->lang_boost[li][0] ? pr->lang_boost[li] : "auto");
    char start[512];
    snprintf(start, sizeof start,
             "{\"event\":\"task_start\",\"model\":\"%s\",\"language_boost\":\"%s\","
             "\"voice_setting\":{\"voice_id\":\"%s\",\"speed\":%.2f,\"vol\":1,\"pitch\":0},"
             "\"audio_setting\":{\"sample_rate\":%u,\"format\":\"pcm\",\"channel\":1}}",
             pr->tts_model, boost, voice, (double)(p->speed > 0 ? p->speed : 1.0f), (unsigned)(p->sample_rate ? p->sample_rate : 16000u));
    net_ws_send_text(s->ws, start, strlen(start));
    if (!signal_wait(s->started, 8000) || atomic_load(&s->failed)) {
        LOGE("tts task_start not acknowledged");
        net_ws_close(s->ws); mem_release(s);
        return ESP_ERR_TIMEOUT;
    }
    LOGI("tts session ready (%s, %s) in %lld ms", voice, boost, (long long)((clock_us() - t0) / 1000));
    *out = s;
    return ESP_OK;
}

static esp_err_t mm_tts_text(tts_session_t *s, const char *utf8, size_t n)
{
    strbuf_t b; strbuf_init(&b);
    strbuf_puts(&b, "{\"event\":\"task_continue\",\"text\":\"");
    char tmp[512]; size_t c = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
    memcpy(tmp, utf8, c); tmp[c] = '\0';
    strbuf_append_json_escaped(&b, tmp);
    strbuf_puts(&b, "\"}");
    for (size_t i = 0; i < c; i++) if (((unsigned char)tmp[i] & 0xC0) != 0x80) s->chars_sent++;
    esp_err_t err = net_ws_send_text(s->ws, b.s, b.len);
    strbuf_free(&b);
    return err;
}

static esp_err_t mm_tts_flush(tts_session_t *s)
{
    static const char F[] = "{\"event\":\"task_flush\"}";
    return net_ws_send_text(s->ws, F, sizeof F - 1);
}

static esp_err_t mm_tts_cancel(tts_session_t *s)
{
    static const char C[] = "{\"event\":\"task_cancel\"}";
    return net_ws_send_text(s->ws, C, sizeof C - 1);
}

static void mm_tts_close(tts_session_t *s)
{
    if (!s) return;
    static const char F[] = "{\"event\":\"task_finish\"}";
    if (net_ws_is_connected(s->ws)) { net_ws_send_text(s->ws, F, sizeof F - 1); clock_sleep_ms(50); }
    net_ws_close(s->ws);
    mem_release(s);
}

const provider_vtable_t provider_minimax = {
    .id = VENDOR,
    .roles = PROVIDER_ROLE_ASR | PROVIDER_ROLE_CHAT | PROVIDER_ROLE_TTS | PROVIDER_ROLE_IMAGE,
    .asr_open = mm_asr_open, .asr_feed = mm_asr_feed, .asr_finish = mm_asr_finish, .asr_close = mm_asr_close,
    .chat_open = mm_chat_open, .chat_send = mm_chat_send, .chat_cancel = mm_chat_cancel, .chat_close = mm_chat_close,
    .tts_open = mm_tts_open, .tts_text = mm_tts_text, .tts_flush = mm_tts_flush, .tts_cancel = mm_tts_cancel, .tts_close = mm_tts_close,
};
