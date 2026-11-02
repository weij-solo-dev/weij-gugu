// provider_fake.c — a scripted vendor for tests and the replay harness: deterministic, offline, instant.
// ASR returns a fixed transcript, CHAT streams a reply with tags in small deltas, TTS turns text into
// a quiet tone whose length tracks the character count.
#define LOG_TAG "provider"
#include "provider/provider.h"
#include "provider_internal.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *s_asr_text = "你好呀，今天去公园看到鸭子了。";
static const char *s_chat_reply = "[happy] 哇，看到鸭子啦！[curious] 鸭子在游泳吗？[coo] 我最喜欢看鸭子了。";

void provider_fake_script(const char *asr_text, const char *chat_reply)
{
    if (asr_text) s_asr_text = asr_text;
    if (chat_reply) s_chat_reply = chat_reply;
}

struct asr_session { asr_events_t ev; void *user; size_t samples; };
struct chat_session { chat_events_t ev; void *user; bool cancelled; };
struct tts_session { tts_events_t ev; void *user; tts_params_t p; strbuf_t text; size_t chars_total; };

static esp_err_t f_asr_open(const asr_params_t *p, const asr_events_t *ev, void *user, asr_session_t **out)
{
    struct asr_session *s = calloc(1, sizeof *s); s->ev = *ev; s->user = user; *out = s; return ESP_OK;
}
static esp_err_t f_asr_feed(asr_session_t *s, const int16_t *pcm, size_t n) { s->samples += n; return ESP_OK; }
static esp_err_t f_asr_finish(asr_session_t *s)
{
    provider_usage_t u = { .audio_ms = (uint32_t)(s->samples / 16) };
    const char *env = getenv("GUGU_FAKE_ASR");
    s->ev.on_final(env ? env : s_asr_text, LANG_ZH, &u, s->user);
    s->samples = 0;
    return ESP_OK;
}
static void f_asr_close(asr_session_t *s) { free(s); }

static esp_err_t f_chat_open(const chat_events_t *ev, void *user, chat_session_t **out)
{
    struct chat_session *s = calloc(1, sizeof *s); s->ev = *ev; s->user = user; *out = s; return ESP_OK;
}
static esp_err_t f_chat_send(chat_session_t *s, const chat_request_t *req)
{
    const char *env = getenv("GUGU_FAKE_REPLY");
    const char *r = env ? env : s_chat_reply;
    size_t n = strlen(r);
    for (size_t i = 0; i < n && !s->cancelled;) {           // deltas of 1–3 UTF-8 characters
        size_t k = 0;
        for (int c = 0; c < 2 && i + k < n; c++) {
            unsigned char b = (unsigned char)r[i + k];
            k += b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : 4;
        }
        if (i + k > n) k = n - i;
        s->ev.on_delta(r + i, k, s->user);
        i += k;
    }
    provider_usage_t u = { .in_tokens = 500, .out_tokens = (uint32_t)(n / 2), .cached_tokens = 400 };
    s->ev.on_done(s->cancelled ? "cancelled" : "end_turn", &u, s->user);
    return ESP_OK;
}
static void f_chat_cancel(chat_session_t *s) { s->cancelled = true; }
static void f_chat_close(chat_session_t *s) { free(s); }

static esp_err_t f_tts_open(const tts_params_t *p, const tts_events_t *ev, void *user, tts_session_t **out)
{
    struct tts_session *s = calloc(1, sizeof *s); s->ev = *ev; s->user = user; s->p = *p; strbuf_init(&s->text); *out = s; return ESP_OK;
}
static esp_err_t f_tts_text(tts_session_t *s, const char *utf8, size_t n) { strbuf_append(&s->text, utf8, n); return ESP_OK; }
static size_t utf8_chars(const char *s) { size_t c = 0; for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) c++; return c; }
static esp_err_t f_tts_flush(tts_session_t *s)
{
    if (s->text.len == 0) { provider_usage_t u = { 0 }; if (s->ev.on_flushed) s->ev.on_flushed(&u, s->user); return ESP_OK; }
    size_t chars = utf8_chars(s->text.s);
    s->chars_total += chars;
    if (s->ev.on_sentence_start) s->ev.on_sentence_start(s->text.s, s->user);
    uint32_t sr = s->p.sample_rate ? s->p.sample_rate : 16000;
    size_t samples = (size_t)(sr * (0.18 * chars + 0.25));   // ~180 ms per character
    int16_t chunk[320];
    for (size_t i = 0; i < samples; i += 320) {
        size_t n = samples - i < 320 ? samples - i : 320;
        for (size_t k = 0; k < n; k++) chunk[k] = (int16_t)(1500.0 * sin(2 * M_PI * 440.0 * (double)(i + k) / sr));
        s->ev.on_audio(chunk, n, s->user);
    }
    if (s->ev.on_sentence_end) s->ev.on_sentence_end(s->user);
    provider_usage_t u = { .chars = (uint32_t)chars, .audio_ms = (uint32_t)(samples * 1000 / sr) };
    if (s->ev.on_flushed) s->ev.on_flushed(&u, s->user);
    strbuf_clear(&s->text);
    return ESP_OK;
}
static esp_err_t f_tts_cancel(tts_session_t *s) { strbuf_clear(&s->text); return ESP_OK; }
static void f_tts_close(tts_session_t *s) { strbuf_free(&s->text); free(s); }

const provider_vtable_t provider_fake = {
    .id = "fake",
    .roles = PROVIDER_ROLE_ASR | PROVIDER_ROLE_CHAT | PROVIDER_ROLE_TTS,
    .asr_open = f_asr_open, .asr_feed = f_asr_feed, .asr_finish = f_asr_finish, .asr_close = f_asr_close,
    .chat_open = f_chat_open, .chat_send = f_chat_send, .chat_cancel = f_chat_cancel, .chat_close = f_chat_close,
    .tts_open = f_tts_open, .tts_text = f_tts_text, .tts_flush = f_tts_flush, .tts_cancel = f_tts_cancel, .tts_close = f_tts_close,
};
