// provider_httptts.c — the voice for vendors that speak over plain HTTP, one request per segment (OpenAI's speech
// endpoint, Gemini's streamed interactions). The engine flushes a segment at every sentence and waits for its
// on_flushed, in order; it must never block on the network. So each session keeps the flushed segments in a queue
// and a worker task speaks them one by one through the vendor's speak(). A cancel bumps a generation: the segment
// in flight is cut short (the vendor's abort), queued ones are dropped, and nothing from them reaches the engine.
#define LOG_TAG "httptts"
#include "provider_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/task.h"
#include "foundation/resample.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE 16
#define CHUNK 1024                        // samples converted per call to the engine

struct http_tts {
    tts_events_t ev; void *user; tts_params_t p;
    http_tts_speak_fn speak; void (*abort)(void *vendor); void *vendor;
    mutex_t lock;
    strbuf_t pending;                     // text since the last flush
    struct { char *text; unsigned gen; } q[QUEUE]; int q_head, q_count;
    atomic_uint gen;                      // bumped by every cancel
    unsigned cur_gen;                     // the generation of the segment being spoken
    signal_t wake, gone;
    atomic_bool closing;
    resample_3to2_t rs; uint8_t carry; bool has_carry;
    int16_t *in, *out;                    // conversion scratch (PSRAM)
};

static void worker(void *arg)
{
    http_tts_t *t = arg;
    for (;;) {
        signal_wait(t->wake, 500);
        if (atomic_load(&t->closing)) break;
        for (;;) {
            char *text = NULL; unsigned gen = 0;
            mutex_lock(t->lock);
            if (t->q_count) { text = t->q[t->q_head].text; gen = t->q[t->q_head].gen; t->q_head = (t->q_head + 1) % QUEUE; t->q_count--; }
            mutex_unlock(t->lock);
            if (!text) break;
            if (gen != atomic_load(&t->gen) || atomic_load(&t->closing)) { free(text); continue; }   // cancelled while queued
            t->cur_gen = gen; t->has_carry = false; resample_3to2_init(&t->rs);
            if (t->ev.on_sentence_start) t->ev.on_sentence_start(text, t->user);
            provider_error_t e = { 0 };
            uint32_t chars = 0;
            for (const char *c = text; *c; c++) if (((unsigned char)*c & 0xC0) != 0x80) chars++;
            esp_err_t err = t->speak(t, text, &e);
            free(text);
            if (gen != atomic_load(&t->gen) || atomic_load(&t->closing)) continue;   // cancelled mid-way: the engine has moved on
            if (err == ESP_OK) { provider_usage_t u = { .chars = chars }; if (t->ev.on_flushed) t->ev.on_flushed(&u, t->user); }
            else { if (!e.kind) provider_error_set(&e, PERR_OTHER, 0, 0, "speech request failed"); if (t->ev.on_error) t->ev.on_error(&e, t->user); }
        }
    }
    signal_set(t->gone);
}

http_tts_t *http_tts_create(const tts_params_t *p, const tts_events_t *ev, void *user, http_tts_speak_fn speak, void (*abort)(void *vendor), void *vendor)
{
    http_tts_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->ev = *ev; t->user = user; t->p = *p; t->speak = speak; t->abort = abort; t->vendor = vendor;
    if (!t->p.sample_rate) t->p.sample_rate = 16000;
    t->in = mem_alloc_big(CHUNK * sizeof(int16_t)); t->out = mem_alloc_big((CHUNK * 2 / 3 + 4) * sizeof(int16_t));
    t->lock = mutex_create(); t->wake = signal_create(); t->gone = signal_create();
    strbuf_init(&t->pending);
    if (!t->in || !t->out || !task_spawn("gugu_provider_tts", worker, t, 7168, 5, 1)) {
        LOGE("could not start the speech worker");
        mem_release(t->in); mem_release(t->out); mutex_free(t->lock); signal_free(t->wake); signal_free(t->gone); free(t);
        return NULL;
    }
    return t;
}

void *http_tts_vendor(http_tts_t *t) { return t->vendor; }
const tts_params_t *http_tts_params(http_tts_t *t) { return &t->p; }
bool http_tts_stale(http_tts_t *t) { return t->cur_gen != atomic_load(&t->gen) || atomic_load(&t->closing); }

void http_tts_pcm(http_tts_t *t, const uint8_t *bytes, size_t n, uint32_t rate)
{
    while (n > 0 && !http_tts_stale(t)) {
        size_t k = 0;                                          // bytes → whole samples, an odd byte carried to the next call
        if (t->has_carry) { t->in[k++] = (int16_t)(t->carry | (bytes[0] << 8)); bytes++; n--; t->has_carry = false; }
        size_t take = n / 2; if (take > CHUNK - k) take = CHUNK - k;
        memcpy(t->in + k, bytes, take * 2); k += take; bytes += take * 2; n -= take * 2;
        if (n == 1 && k < CHUNK) { t->carry = bytes[0]; t->has_carry = true; n = 0; }
        if (!k) continue;
        if (rate == 24000 && t->p.sample_rate == 16000) {
            size_t m = resample_3to2(&t->rs, t->in, k, t->out);
            if (m && t->ev.on_audio) t->ev.on_audio(t->out, m, t->user);
        } else if (t->ev.on_audio) t->ev.on_audio(t->in, k, t->user);
    }
}

esp_err_t http_tts_text(http_tts_t *t, const char *utf8, size_t n)
{
    mutex_lock(t->lock); strbuf_append(&t->pending, utf8, n); mutex_unlock(t->lock);
    return ESP_OK;
}

esp_err_t http_tts_flush(http_tts_t *t)
{
    mutex_lock(t->lock);
    esp_err_t r = ESP_OK;
    if (!t->pending.len) {                                     // nothing to say: the engine still waits for its flush
        mutex_unlock(t->lock);
        provider_usage_t u = { 0 }; if (t->ev.on_flushed) t->ev.on_flushed(&u, t->user);
        return ESP_OK;
    }
    if (t->q_count == QUEUE) r = ESP_ERR_NO_MEM;
    else {
        int i = (t->q_head + t->q_count) % QUEUE;
        t->q[i].text = strdup(t->pending.s); t->q[i].gen = atomic_load(&t->gen); t->q_count++;
    }
    strbuf_clear(&t->pending);
    mutex_unlock(t->lock);
    signal_set(t->wake);
    return r;
}

esp_err_t http_tts_cancel(http_tts_t *t)
{
    mutex_lock(t->lock);
    atomic_fetch_add(&t->gen, 1);
    while (t->q_count) { free(t->q[t->q_head].text); t->q_head = (t->q_head + 1) % QUEUE; t->q_count--; }
    strbuf_clear(&t->pending);
    mutex_unlock(t->lock);
    if (t->abort) t->abort(t->vendor);
    return ESP_OK;
}

void http_tts_destroy(http_tts_t *t)
{
    if (!t) return;
    atomic_store(&t->closing, true);
    if (t->abort) t->abort(t->vendor);
    signal_set(t->wake);
    if (!signal_wait(t->gone, 8000)) { LOGW("speech worker did not stop: leaving its session behind"); return; }
    while (t->q_count) { free(t->q[t->q_head].text); t->q_head = (t->q_head + 1) % QUEUE; t->q_count--; }
    strbuf_free(&t->pending);
    mem_release(t->in); mem_release(t->out);
    mutex_free(t->lock); signal_free(t->wake); signal_free(t->gone);
    free(t);
}
