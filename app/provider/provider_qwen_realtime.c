// provider_qwen_realtime.c — Alibaba Bailian realtime WebSocket: streaming ASR (qwen3-asr-flash-realtime,
// manual commit = push-to-talk). The speech-to-speech model (qwen3.8-omni-flash-realtime) shares this
// wire and is the designated S2S slot; not implemented in v1.0.
// Written against the public protocol docs (2026-11-02); not yet exercised live (no Bailian key).
#define LOG_TAG "qwen_rt"
#include "provider/provider.h"
#include "provider_internal.h"
#include "net/net.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/hexb64.h"
#include "foundation/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

#define VENDOR "qwen_realtime"
static const provider_preset_t *preset(void) { return provider_preset_get(VENDOR); }
static const char *api_key(void) { return provider_credential(VENDOR, "api_key"); }

struct asr_session {
    asr_events_t ev; void *user; asr_params_t p;
    net_ws_t *ws;
    atomic_bool failed;
    signal_t final_sig;
    char final_text[512];
    uint32_t samples;
    char b64[9000];                // 3200 bytes of PCM → 4268 base64 chars
};

static const char *json_str(const char *msg, const char *key, size_t *len)
{
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(msg, pat); if (!p) return NULL;
    p += strlen(pat); const char *e = p;
    while (*e && !(*e == '"' && e[-1] != '\\')) e++;
    *len = (size_t)(e - p); return p;
}

static void unescape(char *dst, size_t cap, const char *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (src[i] == '\\' && i + 1 < n) { i++; dst[o++] = src[i] == 'n' ? '\n' : src[i]; }
        else dst[o++] = src[i];
    }
    dst[o] = '\0';
}

static void rt_text(const char *msg, size_t n, void *user)
{
    struct asr_session *s = user; (void)n;
    size_t tl; const char *type = json_str(msg, "type", &tl);
    if (!type) return;
    if (!strncmp(type, "conversation.item.input_audio_transcription.completed", tl)) {
        size_t l; const char *t = json_str(msg, "transcript", &l);
        if (t) unescape(s->final_text, sizeof s->final_text, t, l);
        signal_set(s->final_sig);
    } else if (!strncmp(type, "conversation.item.input_audio_transcription.text", tl)) {
        size_t l; const char *t = json_str(msg, "text", &l);
        if (t && s->ev.on_partial) { char tmp[256]; unescape(tmp, sizeof tmp, t, l); s->ev.on_partial(tmp, s->user); }
    } else if (!strncmp(type, "error", tl)) {
        size_t l; const char *m = json_str(msg, "message", &l);
        char buf[96]; snprintf(buf, sizeof buf, "%.*s", (int)(m ? (l < 90 ? l : 90) : 0), m ? m : "");
        atomic_store(&s->failed, true);
        provider_error_t e; provider_error_set(&e, PERR_OTHER, 200, 0, buf[0] ? buf : "realtime error");
        s->ev.on_error(&e, s->user);
        signal_set(s->final_sig);
    }
}
static void rt_err(const char *what, void *user)
{
    struct asr_session *s = user;
    if (atomic_exchange(&s->failed, true)) return;
    provider_error_t e; provider_error_set(&e, PERR_NETWORK, 0, 0, what); s->ev.on_error(&e, s->user);
    signal_set(s->final_sig);
}
static void rt_closed(int code, const char *reason, void *user) { (void)code; (void)reason; signal_set(((struct asr_session *)user)->final_sig); }

static esp_err_t rt_asr_open(const asr_params_t *p, const asr_events_t *ev, void *user, asr_session_t **out)
{
    const provider_preset_t *pr = preset();
    struct asr_session *s = mem_alloc_big(sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    s->ev = *ev; s->user = user; s->p = *p; s->final_sig = signal_create();
    char bearer[300]; snprintf(bearer, sizeof bearer, "Bearer %s", api_key() ? api_key() : "");
    net_header_t h[2] = { { "Authorization", bearer }, { "OpenAI-Beta", "realtime=v1" } };
    net_ws_params_t wp = { .url = pr->asr_url, .headers = h, .header_count = 2, .connect_timeout_ms = 6000, .max_message = 64 * 1024 };
    net_ws_events_t we = { .on_text = rt_text, .on_error = rt_err, .on_closed = rt_closed };
    esp_err_t err = net_ws_open(&wp, &we, s, &s->ws);
    if (err != ESP_OK) { signal_free(s->final_sig); mem_release(s); return err; }
    char upd[320];
    const char *lang = p->lang_hint == LANG_UNKNOWN ? "" : lang_code(p->lang_hint);
    snprintf(upd, sizeof upd, "{\"type\":\"session.update\",\"session\":{\"modalities\":[\"text\"],\"input_audio_format\":\"pcm\",\"sample_rate\":%u,\"turn_detection\":null%s%s%s}}",
             (unsigned)(p->sample_rate ? p->sample_rate : 16000u), lang[0] ? ",\"input_audio_transcription\":{\"language\":\"" : "", lang, lang[0] ? "\"}" : "");
    net_ws_send_text(s->ws, upd, strlen(upd));
    LOGI("realtime asr session open");
    *out = s;
    return ESP_OK;
}

static esp_err_t rt_asr_feed(asr_session_t *s, const int16_t *pcm, size_t n)
{
    // 3200-byte chunks (≈100 ms) as the docs suggest; the engine already feeds 20 ms frames, so coalesce.
    static const size_t CHUNK = 1600;
    for (size_t i = 0; i < n; i += CHUNK) {
        size_t k = n - i < CHUNK ? n - i : CHUNK;
        size_t bl = b64_encode((const uint8_t *)(pcm + i), k * 2, s->b64, sizeof s->b64);
        strbuf_t b; strbuf_init(&b);
        strbuf_puts(&b, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"");
        strbuf_append(&b, s->b64, bl);
        strbuf_puts(&b, "\"}");
        esp_err_t err = net_ws_send_text(s->ws, b.s, b.len);
        strbuf_free(&b);
        if (err != ESP_OK) return err;
    }
    s->samples += (uint32_t)n;
    return ESP_OK;
}

static esp_err_t rt_asr_finish(asr_session_t *s)
{
    static const char C[] = "{\"type\":\"input_audio_buffer.commit\"}";
    s->final_text[0] = '\0';
    int64_t t0 = clock_us();
    net_ws_send_text(s->ws, C, sizeof C - 1);
    bool got = signal_wait(s->final_sig, 8000);
    if (atomic_load(&s->failed)) return ESP_FAIL;
    if (!got) { provider_error_t e; provider_error_set(&e, PERR_TIMEOUT, 0, 0, "no transcript"); s->ev.on_error(&e, s->user); return ESP_ERR_TIMEOUT; }
    provider_usage_t u = { .audio_ms = s->samples / 16 };
    LOGI("realtime asr final in %lld ms", (long long)((clock_us() - t0) / 1000));
    s->ev.on_final(s->final_text, s->p.lang_hint, &u, s->user);
    s->samples = 0;
    return ESP_OK;
}

static void rt_asr_close(asr_session_t *s)
{
    if (!s) return;
    static const char F[] = "{\"type\":\"session.finish\"}";
    if (net_ws_is_connected(s->ws)) { net_ws_send_text(s->ws, F, sizeof F - 1); clock_sleep_ms(50); }
    net_ws_close(s->ws);
    signal_free(s->final_sig);
    mem_release(s);
}

const provider_vtable_t provider_qwen_realtime = {
    .id = VENDOR,
    .roles = PROVIDER_ROLE_ASR,
    .asr_open = rt_asr_open, .asr_feed = rt_asr_feed, .asr_finish = rt_asr_finish, .asr_close = rt_asr_close,
};
