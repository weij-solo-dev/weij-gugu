// provider_volc.c — Volcengine (火山引擎): streaming ASR 2.0 (sauc nostream endpoint: push-to-talk,
// auto language), bidirectional TTS 2.0 (seed-tts-2.0), chat over Ark's OpenAI-compatible stream.
// Both speech APIs speak a 4-byte-header binary frame protocol over WebSocket, implemented here.
// Written against the public protocol docs (2026-11-02); not yet exercised live (no Volcengine key).
#define LOG_TAG "volc"
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

#define VENDOR "volc"
static const provider_preset_t *preset(void) { return provider_preset_get(VENDOR); }
static const char *speech_key(void) { return provider_credential(VENDOR, "api_key"); }
static const char *ark_key(void) { return provider_credential(VENDOR, "ark_api_key"); }

// ---- the binary frame (shared by ASR and TTS) ----
// byte0 version|header_size, byte1 type|flags, byte2 serialization|compression, byte3 reserved,
// [event int32] [session: uint32 len + bytes] [sequence int32] uint32 payload_size, payload. Big-endian.
enum { MT_FULL_CLIENT = 0x1, MT_AUDIO_ONLY = 0x2, MT_FULL_SERVER = 0x9, MT_AUDIO_SERVER = 0xB, MT_ERROR = 0xF };
enum { FL_NONE = 0x0, FL_SEQ = 0x1, FL_LAST = 0x2, FL_LAST_SEQ = 0x3, FL_EVENT = 0x4 };
enum { SER_RAW = 0x0, SER_JSON = 0x1 };

static void put32(strbuf_t *b, uint32_t v) { char x[4] = { (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v }; strbuf_append(b, x, 4); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

size_t volc_frame_build(strbuf_t *out, int type, int flags, int ser, int event, const char *session, int32_t seq,
                        const void *payload, size_t n)
{
    strbuf_clear(out);
    char h[4] = { 0x11, (char)((type << 4) | flags), (char)(ser << 4), 0 };
    strbuf_append(out, h, 4);
    if (flags & FL_EVENT) put32(out, (uint32_t)event);
    if (session) { put32(out, (uint32_t)strlen(session)); strbuf_puts(out, session); }
    if (flags == FL_SEQ || flags == FL_LAST_SEQ) put32(out, (uint32_t)seq);
    put32(out, (uint32_t)n);
    if (n) strbuf_append(out, payload, n);
    return out->len;
}

bool volc_frame_parse(const uint8_t *d, size_t n, volc_frame_t *f)
{
    memset(f, 0, sizeof *f);
    if (n < 4) return false;
    size_t hs = (size_t)(d[0] & 0x0F) * 4;
    f->type = d[1] >> 4; f->flags = d[1] & 0x0F; f->ser = d[2] >> 4;
    size_t p = hs;
    if (f->type == MT_ERROR) {
        if (p + 8 > n) return false;
        f->err_code = get32(d + p); p += 4;
        uint32_t ml = get32(d + p); p += 4;
        if (p + ml > n) return false;
        f->payload = d + p; f->n = ml; return true;
    }
    if (f->flags & FL_EVENT) { if (p + 4 > n) return false; f->event = (int)get32(d + p); p += 4; }
    if (f->flags & FL_EVENT) {                       // server frames with an event carry a session id (may be empty)
        if (p + 4 > n) return false;
        uint32_t sl = get32(d + p); p += 4 + sl;
        if (p > n) return false;
    }
    if (f->flags == FL_SEQ || f->flags == FL_LAST_SEQ) { if (p + 4 > n) return false; f->seq = (int32_t)get32(d + p); p += 4; }
    if (p + 4 > n) return false;
    uint32_t pl = get32(d + p); p += 4;
    if (p + pl > n) pl = (uint32_t)(n - p);
    f->payload = d + p; f->n = pl;
    return true;
}

static void uuid4(char out[40])
{
    uint32_t a = (uint32_t)clock_us() ^ (uint32_t)rand(), b = (uint32_t)rand(), c = (uint32_t)rand(), e = (uint32_t)rand();
    snprintf(out, 40, "%08x-%04x-4%03x-%04x-%08x%04x", (unsigned)a, (unsigned)(b & 0xFFFF), (unsigned)(b >> 20), (unsigned)(0x8000 | (c & 0x3FFF)), (unsigned)e, (unsigned)(c >> 16));
}

static const char *json_str(const char *msg, size_t n, const char *key, size_t *len)
{
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *end = msg + n;
    const char *p = strstr(msg, pat); if (!p || p >= end) return NULL;
    p += strlen(pat); const char *e = p;
    while (e < end && !(*e == '"' && e[-1] != '\\')) e++;
    *len = (size_t)(e - p); return p;
}

// ------------------------------------------------------------------- ASR ----
struct asr_session {
    asr_events_t ev; void *user; asr_params_t p;
    net_ws_t *ws;
    strbuf_t frame;
    int32_t seq;
    atomic_bool failed;
    signal_t final_sig;
    char final_text[512];
    uint32_t samples;
    int16_t chunk[3200]; size_t chunk_len;     // 200 ms packets
};

static void asr_on_bin(const uint8_t *d, size_t n, void *user)
{
    struct asr_session *s = user;
    volc_frame_t f;
    if (!volc_frame_parse(d, n, &f)) return;
    if (f.type == MT_ERROR) {
        char buf[96]; snprintf(buf, sizeof buf, "asr %u: %.*s", (unsigned)f.err_code, (int)(f.n < 70 ? f.n : 70), (const char *)f.payload);
        atomic_store(&s->failed, true);
        provider_error_t e; provider_error_set(&e, f.err_code == 45000002 ? PERR_OTHER : PERR_OTHER, 200, (int)f.err_code, buf);
        s->ev.on_error(&e, s->user); signal_set(s->final_sig); return;
    }
    if (f.type != MT_FULL_SERVER || f.ser != SER_JSON) return;
    size_t tl; const char *t = json_str((const char *)f.payload, f.n, "text", &tl);
    if (t) {
        size_t c = tl < sizeof s->final_text - 1 ? tl : sizeof s->final_text - 1;
        memcpy(s->final_text, t, c); s->final_text[c] = '\0';
        if (s->ev.on_partial && !(f.flags & FL_LAST)) s->ev.on_partial(s->final_text, s->user);
    }
    if (f.flags & FL_LAST || f.seq < 0) signal_set(s->final_sig);
}
static void asr_on_err(const char *what, void *user) { struct asr_session *s = user; if (atomic_exchange(&s->failed, true)) return; provider_error_t e; provider_error_set(&e, PERR_NETWORK, 0, 0, what); s->ev.on_error(&e, s->user); signal_set(s->final_sig); }
static void asr_on_closed(int code, const char *r, void *user) { (void)code; (void)r; signal_set(((struct asr_session *)user)->final_sig); }

static esp_err_t v_asr_open(const asr_params_t *p, const asr_events_t *ev, void *user, asr_session_t **out)
{
    const provider_preset_t *pr = preset();
    struct asr_session *s = mem_alloc_big(sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    s->ev = *ev; s->user = user; s->p = *p; s->final_sig = signal_create(); strbuf_init(&s->frame);
    char req_id[40], conn_id[40]; uuid4(req_id); uuid4(conn_id);
    net_header_t h[5] = { { "X-Api-Key", speech_key() ? speech_key() : "" }, { "X-Api-Resource-Id", "volc.seedasr.sauc.duration" },
                          { "X-Api-Request-Id", req_id }, { "X-Api-Sequence", "-1" }, { "X-Api-Connect-Id", conn_id } };
    net_ws_params_t wp = { .url = pr->asr_url, .headers = h, .header_count = 5, .connect_timeout_ms = 6000, .max_message = 64 * 1024 };
    net_ws_events_t we = { .on_binary = asr_on_bin, .on_error = asr_on_err, .on_closed = asr_on_closed };
    esp_err_t err = net_ws_open(&wp, &we, s, &s->ws);
    if (err != ESP_OK) { mem_release(s); return err; }
    // first frame: the full client request
    char body[400];
    const char *lang = p->lang_hint == LANG_ZH ? "zh-CN" : p->lang_hint == LANG_EN ? "en-US" : p->lang_hint == LANG_YUE ? "yue-CN" : "";
    snprintf(body, sizeof body,
             "{\"user\":{\"uid\":\"gugu\"},\"audio\":{\"format\":\"pcm\",\"codec\":\"raw\",\"rate\":16000,\"bits\":16,\"channel\":1%s%s%s},"
             "\"request\":{\"model_name\":\"bigmodel\",\"enable_itn\":true,\"enable_punc\":true,\"result_type\":\"full\"%s}}",
             lang[0] ? ",\"language\":\"" : "", lang, lang[0] ? "\"" : "", lang[0] ? "" : ",\"enable_auto_lang\":true");
    s->seq = 1;
    volc_frame_build(&s->frame, MT_FULL_CLIENT, FL_SEQ, SER_JSON, 0, NULL, s->seq++, body, strlen(body));
    net_ws_send_binary(s->ws, (const uint8_t *)s->frame.s, s->frame.len);
    LOGI("volc asr session open");
    *out = s;
    return ESP_OK;
}

static esp_err_t send_chunk(struct asr_session *s, bool last)
{
    volc_frame_build(&s->frame, MT_AUDIO_ONLY, last ? FL_LAST_SEQ : FL_SEQ, SER_RAW, 0, NULL, last ? -s->seq : s->seq, s->chunk, s->chunk_len * 2);
    s->seq++;
    esp_err_t err = net_ws_send_binary(s->ws, (const uint8_t *)s->frame.s, s->frame.len);
    s->chunk_len = 0;
    return err;
}

static esp_err_t v_asr_feed(asr_session_t *s, const int16_t *pcm, size_t n)
{
    s->samples += (uint32_t)n;
    while (n) {
        size_t room = 3200 - s->chunk_len, k = n < room ? n : room;
        memcpy(s->chunk + s->chunk_len, pcm, k * 2);
        s->chunk_len += k; pcm += k; n -= k;
        if (s->chunk_len == 3200) { esp_err_t err = send_chunk(s, false); if (err != ESP_OK) return err; }
    }
    return ESP_OK;
}

static esp_err_t v_asr_finish(asr_session_t *s)
{
    s->final_text[0] = '\0';
    send_chunk(s, true);                                   // the last packet, even if short
    int64_t t0 = clock_us();
    bool got = signal_wait(s->final_sig, 8000);
    if (atomic_load(&s->failed)) return ESP_FAIL;
    if (!got) { provider_error_t e; provider_error_set(&e, PERR_TIMEOUT, 0, 0, "no transcript"); s->ev.on_error(&e, s->user); return ESP_ERR_TIMEOUT; }
    provider_usage_t u = { .audio_ms = s->samples / 16 };
    LOGI("volc asr final in %lld ms", (long long)((clock_us() - t0) / 1000));
    s->ev.on_final(s->final_text, s->p.lang_hint, &u, s->user);
    s->samples = 0;
    return ESP_OK;
}

static void v_asr_close(asr_session_t *s) { if (!s) return; net_ws_close(s->ws); strbuf_free(&s->frame); mem_release(s); }

// ------------------------------------------------------------------ CHAT ----
struct chat_session { chat_events_t ev; void *user; net_http_t *http; wire_openai_t wire; sse_parser_t sse; atomic_bool cancelled; };
static void c_sse(const char *event, const char *data, void *user) { struct chat_session *s = user; wire_openai_event(&s->wire, event, data); }
static void c_body(const uint8_t *chunk, size_t n, void *user) { struct chat_session *s = user; sse_feed(&s->sse, (const char *)chunk, n); }
static esp_err_t v_chat_open(const chat_events_t *ev, void *user, chat_session_t **out)
{
    struct chat_session *s = calloc(1, sizeof *s); if (!s) return ESP_ERR_NO_MEM;
    s->ev = *ev; s->user = user;
    esp_err_t err = net_http_open(preset()->chat_url, &s->http); if (err != ESP_OK) { free(s); return err; }
    net_http_prewarm(s->http, 6000); *out = s; return ESP_OK;
}
static esp_err_t v_chat_send(chat_session_t *s, const chat_request_t *req)
{
    const provider_preset_t *p = preset();
    strbuf_t body; strbuf_init(&body);
    int eff = req->effort < 3 ? req->effort : 0;
    wire_openai_build(&body, p->chat_model, req, p->chat_extra[eff][0] ? p->chat_extra[eff] : NULL);
    wire_openai_init(&s->wire, &s->ev, s->user); sse_init(&s->sse, c_sse, s); atomic_store(&s->cancelled, false);
    char bearer[300]; snprintf(bearer, sizeof bearer, "Bearer %s", ark_key() ? ark_key() : "");
    net_header_t h[2] = { { "Authorization", bearer }, { "Content-Type", "application/json" } };
    net_http_request_t r = { .method = "POST", .url = p->chat_url, .headers = h, .header_count = 2, .body = (const uint8_t *)body.s, .body_len = body.len, .timeout_ms = 12000, .on_body = c_body, .user = s };
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
static void v_chat_cancel(chat_session_t *s) { atomic_store(&s->cancelled, true); net_http_cancel(s->http); }
static void v_chat_close(chat_session_t *s) { if (!s) return; net_http_close(s->http); free(s); }

// ------------------------------------------------------------------- TTS ----
// StartConnection → ConnectionStarted → StartSession → SessionStarted; each segment is one TaskRequest;
// TTSSentenceEnd closes a segment (reported as flushed). FinishSession/FinishConnection at close.
struct tts_session {
    tts_events_t ev; void *user; tts_params_t p;
    net_ws_t *ws;
    strbuf_t frame;
    char session_id[40];
    signal_t conn_started, sess_started;
    atomic_bool failed;
    uint32_t chars_sent, chars_reported;
};

static void tts_on_bin(const uint8_t *d, size_t n, void *user)
{
    struct tts_session *s = user;
    volc_frame_t f;
    if (!volc_frame_parse(d, n, &f)) return;
    if (f.type == MT_ERROR) {
        char buf[96]; snprintf(buf, sizeof buf, "tts %u: %.*s", (unsigned)f.err_code, (int)(f.n < 70 ? f.n : 70), (const char *)f.payload);
        atomic_store(&s->failed, true);
        provider_error_t e; provider_error_set(&e, PERR_OTHER, 200, (int)f.err_code, buf);
        s->ev.on_error(&e, s->user); signal_set(s->conn_started); signal_set(s->sess_started); return;
    }
    switch (f.event) {
    case 50: signal_set(s->conn_started); break;                       // ConnectionStarted
    case 150: signal_set(s->sess_started); break;                      // SessionStarted
    case 350: if (s->ev.on_sentence_start) s->ev.on_sentence_start(NULL, s->user); break;
    case 352: if (f.n >= 2) s->ev.on_audio((const int16_t *)f.payload, f.n / 2, s->user); break;
    case 351: {                                                         // TTSSentenceEnd = one segment voiced
        provider_usage_t u = { .chars = s->chars_sent - s->chars_reported }; s->chars_reported = s->chars_sent;
        if (s->ev.on_sentence_end) s->ev.on_sentence_end(s->user);
        if (s->ev.on_flushed) s->ev.on_flushed(&u, s->user);
        break;
    }
    case 51: case 153: {                                                // ConnectionFailed / SessionFailed
        atomic_store(&s->failed, true);
        provider_error_t e; provider_error_set(&e, PERR_OTHER, 200, f.event, f.event == 51 ? "connection failed" : "session failed");
        s->ev.on_error(&e, s->user); signal_set(s->conn_started); signal_set(s->sess_started);
        break;
    }
    default: break;
    }
}
static void tts_on_err(const char *what, void *user) { struct tts_session *s = user; if (atomic_exchange(&s->failed, true)) return; provider_error_t e; provider_error_set(&e, PERR_NETWORK, 0, 0, what); s->ev.on_error(&e, s->user); signal_set(s->conn_started); signal_set(s->sess_started); }
static void tts_on_closed(int code, const char *r, void *user) { (void)code; (void)r; struct tts_session *s = user; signal_set(s->conn_started); signal_set(s->sess_started); }

static esp_err_t v_tts_open(const tts_params_t *p, const tts_events_t *ev, void *user, tts_session_t **out)
{
    const provider_preset_t *pr = preset();
    struct tts_session *s = calloc(1, sizeof *s); if (!s) return ESP_ERR_NO_MEM;
    s->ev = *ev; s->user = user; s->p = *p; strbuf_init(&s->frame);
    s->conn_started = signal_create(); s->sess_started = signal_create();
    char conn_id[40]; uuid4(conn_id); uuid4(s->session_id);
    net_header_t h[3] = { { "X-Api-Key", speech_key() ? speech_key() : "" }, { "X-Api-Resource-Id", "seed-tts-2.0" }, { "X-Api-Connect-Id", conn_id } };
    net_ws_params_t wp = { .url = pr->tts_url, .headers = h, .header_count = 3, .connect_timeout_ms = 6000, .max_message = 256 * 1024 };
    net_ws_events_t we = { .on_binary = tts_on_bin, .on_error = tts_on_err, .on_closed = tts_on_closed };
    esp_err_t err = net_ws_open(&wp, &we, s, &s->ws);
    if (err != ESP_OK) { free(s); return err; }
    volc_frame_build(&s->frame, MT_FULL_CLIENT, FL_EVENT, SER_JSON, 1, NULL, 0, "{}", 2);                 // StartConnection
    net_ws_send_binary(s->ws, (const uint8_t *)s->frame.s, s->frame.len);
    if (!signal_wait(s->conn_started, 6000) || atomic_load(&s->failed)) { net_ws_close(s->ws); free(s); return ESP_ERR_TIMEOUT; }
    const char *voice = provider_voice(pr, p->lang, p->voice_id);
    char body[512];
    snprintf(body, sizeof body, "{\"user\":{\"uid\":\"gugu\"},\"event\":100,\"namespace\":\"BidirectionalTTS\",\"req_params\":{\"speaker\":\"%s\","
             "\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":%u},\"additions\":\"{\\\"disable_markdown_filter\\\":false}\"}}",
             voice, (unsigned)(p->sample_rate ? p->sample_rate : 16000u));
    volc_frame_build(&s->frame, MT_FULL_CLIENT, FL_EVENT, SER_JSON, 100, s->session_id, 0, body, strlen(body));   // StartSession
    net_ws_send_binary(s->ws, (const uint8_t *)s->frame.s, s->frame.len);
    if (!signal_wait(s->sess_started, 6000) || atomic_load(&s->failed)) { net_ws_close(s->ws); free(s); return ESP_ERR_TIMEOUT; }
    LOGI("volc tts session ready (%s)", voice);
    *out = s;
    return ESP_OK;
}

static esp_err_t v_tts_text(tts_session_t *s, const char *utf8, size_t n)
{
    strbuf_t b; strbuf_init(&b);
    strbuf_puts(&b, "{\"event\":200,\"namespace\":\"BidirectionalTTS\",\"req_params\":{\"text\":\"");
    char tmp[512]; size_t c = n < sizeof tmp - 1 ? n : sizeof tmp - 1; memcpy(tmp, utf8, c); tmp[c] = '\0';
    strbuf_append_json_escaped(&b, tmp);
    strbuf_puts(&b, "\"}}");
    for (size_t i = 0; i < c; i++) if (((unsigned char)tmp[i] & 0xC0) != 0x80) s->chars_sent++;
    volc_frame_build(&s->frame, MT_FULL_CLIENT, FL_EVENT, SER_JSON, 200, s->session_id, 0, b.s, b.len);        // TaskRequest
    esp_err_t err = net_ws_send_binary(s->ws, (const uint8_t *)s->frame.s, s->frame.len);
    strbuf_free(&b);
    return err;
}
static esp_err_t v_tts_flush(tts_session_t *s) { (void)s; return ESP_OK; }      // segments end with punctuation; the server voices them per sentence
static esp_err_t v_tts_cancel(tts_session_t *s) { (void)s; return ESP_OK; }
static void v_tts_close(tts_session_t *s)
{
    if (!s) return;
    if (net_ws_is_connected(s->ws)) {
        volc_frame_build(&s->frame, MT_FULL_CLIENT, FL_EVENT, SER_JSON, 102, s->session_id, 0, "{}", 2); net_ws_send_binary(s->ws, (const uint8_t *)s->frame.s, s->frame.len);   // FinishSession
        volc_frame_build(&s->frame, MT_FULL_CLIENT, FL_EVENT, SER_JSON, 2, NULL, 0, "{}", 2); net_ws_send_binary(s->ws, (const uint8_t *)s->frame.s, s->frame.len);               // FinishConnection
        clock_sleep_ms(50);
    }
    net_ws_close(s->ws); strbuf_free(&s->frame); free(s);
}

const provider_vtable_t provider_volc = {
    .id = VENDOR,
    .roles = PROVIDER_ROLE_ASR | PROVIDER_ROLE_CHAT | PROVIDER_ROLE_TTS,
    .asr_open = v_asr_open, .asr_feed = v_asr_feed, .asr_finish = v_asr_finish, .asr_close = v_asr_close,
    .chat_open = v_chat_open, .chat_send = v_chat_send, .chat_cancel = v_chat_cancel, .chat_close = v_chat_close,
    .tts_open = v_tts_open, .tts_text = v_tts_text, .tts_flush = v_tts_flush, .tts_cancel = v_tts_cancel, .tts_close = v_tts_close,
};
