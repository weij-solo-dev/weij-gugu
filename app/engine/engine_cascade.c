// engine_cascade.c — the push-to-talk turn: ASR on release → prompt → streamed LLM → tag parser →
// TTS per segment (flush per segment keeps the segment/emotion pairing exact) → PCM events.
// One task per engine runs the blocking vendor calls; TTS audio arrives on the vendor's own thread.
#define LOG_TAG "engine"
#include "engine_internal.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "net/net.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

enum cmd { CMD_START, CMD_TURN, CMD_END, CMD_QUIT };
struct msg { enum cmd cmd; lang_t lang; char where[96]; char events[96]; };

typedef struct {
    engine_t *e;
    queue_t q;
    mutex_t lock;
    atomic_bool busy, abort, in_call;
    asr_session_t *asr; chat_session_t *chat; tts_session_t *tts;
    lang_t lang_hint, reply_lang;
    // turn state
    engine_timing_t t;
    char transcript[512];
    strbuf_t reply, seg_text;
    mind_tag_t pending_emotion;          // tag parsed before the segment being collected
    mind_tag_t seg_emotion[32];          // emotion per segment index
    char *seg_texts[32];
    atomic_int seg_sent, seg_flushed, seg_playing;
    signal_t tts_flush_sig;
    provider_usage_t u_asr, u_chat, u_tts;
    int tags_seen;
    tag_parser_t tags;
    bool chat_done; provider_error_t chat_err; bool chat_failed;
    bool tts_failed; provider_error_t tts_err;
    bool asr_failed; provider_error_t asr_err;
} cascade_t;

static void emit(cascade_t *c, engine_event_t *ev) { engine_emit(c->e, ev); }

// ---- ASR events ----
static void asr_final(const char *text, lang_t lang, const provider_usage_t *u, void *user)
{
    cascade_t *c = user;
    snprintf(c->transcript, sizeof c->transcript, "%s", text);
    c->u_asr = *u;
    if (lang != LANG_UNKNOWN) c->reply_lang = lang;
    c->t.t1_transcript = clock_us();
}
static void asr_error(const provider_error_t *err, void *user) { cascade_t *c = user; c->asr_failed = true; c->asr_err = *err; }

// ---- TTS events (vendor thread) ----
static void tts_sentence_start(const char *text, void *user) { (void)text; (void)user; }
static void tts_audio(const int16_t *pcm, size_t n, void *user)
{
    cascade_t *c = user;
    if (atomic_load(&c->abort)) return;
    if (!c->t.t3_first_audio) c->t.t3_first_audio = clock_us();
    int seg = atomic_load(&c->seg_playing);
    engine_event_t ev = { .kind = ENGINE_EV_AUDIO, .u.audio = { pcm, n, seg } };
    emit(c, &ev);
}
static void tts_flushed(const provider_usage_t *u, void *user)
{
    cascade_t *c = user;
    c->u_tts.chars += u->chars;
    int done = atomic_load(&c->seg_playing);
    engine_event_t ev = { .kind = ENGINE_EV_SEGMENT_END, .u.segment = { done, c->seg_emotion[done & 31], NULL } };
    emit(c, &ev);
    atomic_fetch_add(&c->seg_flushed, 1);
    int next = atomic_fetch_add(&c->seg_playing, 1) + 1;
    if (next < atomic_load(&c->seg_sent)) {       // the next segment was already sent: it is sounding next
        engine_event_t st = { .kind = ENGINE_EV_SEGMENT_START, .u.segment = { next, c->seg_emotion[next & 31], c->seg_texts[next & 31] } };
        emit(c, &st);
    }
    signal_set(c->tts_flush_sig);
}
static void tts_error(const provider_error_t *err, void *user) { cascade_t *c = user; c->tts_failed = true; c->tts_err = *err; signal_set(c->tts_flush_sig); }

// ---- chat deltas → tag parser → segments ----
static void seg_boundary(cascade_t *c)
{
    if (c->seg_text.len == 0) return;
    int idx = atomic_load(&c->seg_sent);
    c->seg_emotion[idx & 31] = c->pending_emotion;
    free(c->seg_texts[idx & 31]);
    c->seg_texts[idx & 31] = strdup(c->seg_text.s);
    atomic_store(&c->seg_sent, idx + 1);
    if (idx == atomic_load(&c->seg_playing)) {    // nothing ahead of it: this segment sounds next
        engine_event_t st = { .kind = ENGINE_EV_SEGMENT_START, .u.segment = { idx, c->pending_emotion, c->seg_texts[idx & 31] } };
        emit(c, &st);
    }
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_TTS);
    if (c->tts && !c->tts_failed) {
        v->tts_text(c->tts, c->seg_text.s, c->seg_text.len);
        v->tts_flush(c->tts);
    }
    strbuf_clear(&c->seg_text);
}
static void on_tag(mind_tag_t t, void *user)
{
    cascade_t *c = user;
    c->tags_seen++;
    if (mind_tag_is_emotion(t) || t == TAG_PUFF || t == TAG_WAVE) c->pending_emotion = t;
    engine_event_t ev = { .kind = ENGINE_EV_TAG, .u.tag = t };
    emit(c, &ev);
}
static void on_text(const char *s, size_t n, void *user) { cascade_t *c = user; strbuf_append(&c->seg_text, s, n); strbuf_append(&c->reply, s, n); }
static void on_segment_end(void *user) { seg_boundary((cascade_t *)user); }

static void chat_delta(const char *t, size_t n, void *user)
{
    cascade_t *c = user;
    if (atomic_load(&c->abort)) return;
    if (!c->t.t2_first_token) c->t.t2_first_token = clock_us();
    tag_parser_feed(&c->tags, t, n);
}
static void chat_done(const char *stop, const provider_usage_t *u, void *user) { cascade_t *c = user; c->u_chat = *u; c->chat_done = true; (void)stop; }
static void chat_error(const provider_error_t *err, void *user) { cascade_t *c = user; c->chat_failed = true; c->chat_err = *err; }

// ---- the TTS session ----
// The call opens one TTS session and keeps it for every turn. When a turn loses it (the vendor closed it, a read
// failed), it is replaced at once, and again at the start of the next turn if that failed, so one bad turn does not
// spoil the rest of the call. Swaps happen under the lock because an abort cancels the session from another task.
static esp_err_t tts_open_session(cascade_t *c, tts_session_t **out)
{
    tts_params_t tp = { .lang = mind_primary_lang(), .sample_rate = 16000, .speed = 1.0f };   // LANG_UNKNOWN → vendor auto-detect per text
    tts_events_t te = { tts_sentence_start, tts_audio, NULL, tts_flushed, tts_error };
    return provider_for(PROVIDER_ROLE_TTS)->tts_open(&tp, &te, c, out);
}

static bool tts_renew(cascade_t *c)
{
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_TTS);
    mutex_lock(c->lock); tts_session_t *old = c->tts; c->tts = NULL; mutex_unlock(c->lock);
    if (old) v->tts_close(old);
    int64_t t0 = clock_us();
    tts_session_t *fresh = NULL;
    esp_err_t r = tts_open_session(c, &fresh);
    mutex_lock(c->lock); c->tts = r == ESP_OK ? fresh : NULL; c->tts_failed = false; mutex_unlock(c->lock);
    LOGI("tts session renewed: %s in %lld ms", esp_err_to_name(r), (long long)((clock_us() - t0) / 1000));
    return r == ESP_OK;
}

// ---- the turn ----
static void fail(cascade_t *c, const char *stage, const provider_error_t *err)
{
    engine_event_t ev = { .kind = ENGINE_EV_ERROR, .u.error = { *err, stage } };
    LOGW("turn failed at %s: kind %d http %d api %d %s", stage, err->kind, err->http_status, err->api_code, err->message);
    emit(c, &ev);
}

static void run_turn(cascade_t *c, const char *events)
{
    const provider_vtable_t *vasr = provider_for(PROVIDER_ROLE_ASR), *vchat = provider_for(PROVIDER_ROLE_CHAT);
    memset(&c->t, 0, sizeof c->t); c->t.t0_release = clock_us();
    c->transcript[0] = '\0'; c->asr_failed = c->chat_failed = c->chat_done = false;
    strbuf_clear(&c->reply); strbuf_clear(&c->seg_text);
    atomic_store(&c->seg_sent, 0); atomic_store(&c->seg_flushed, 0); atomic_store(&c->seg_playing, 0); c->pending_emotion = TAG_NEUTRAL;
    memset(&c->u_asr, 0, sizeof c->u_asr); memset(&c->u_chat, 0, sizeof c->u_chat); memset(&c->u_tts, 0, sizeof c->u_tts);
    c->tags_seen = 0;

    // 0. a voice to answer with: the last turn may have lost it
    if ((!c->tts || c->tts_failed) && !tts_renew(c)) { provider_error_t e; provider_error_set(&e, PERR_NETWORK, 0, 0, "tts session could not reopen"); fail(c, "tts", &e); return; }

    // 1. ASR
    if (!c->asr) { provider_error_t e; provider_error_set(&e, PERR_OTHER, 0, 0, "no asr session"); fail(c, "asr", &e); return; }
    vasr->asr_finish(c->asr);
    if (atomic_load(&c->abort)) return;
    if (c->asr_failed) { fail(c, "asr", &c->asr_err); return; }
    mind_normalize_transcript(c->transcript);
    if (c->transcript[0] == '\0') {                       // silence, a breath, a tap: nothing to answer, not a failure
        LOGI("nothing heard in %u ms of audio", (unsigned)c->u_asr.audio_ms);
        engine_event_t nh = { .kind = ENGINE_EV_NOTHING_HEARD }; emit(c, &nh); return;
    }
    c->reply_lang = mind_reply_lang(c->reply_lang, c->transcript);
    engine_event_t tev = { .kind = ENGINE_EV_TRANSCRIPT, .u.transcript = { c->transcript, c->reply_lang } };
    emit(c, &tev);
    mind_dialog_add("user", c->transcript);

    // 2. prompt + chat (deltas drive the tag parser, segments drive the TTS)
    mind_prompt_t pr;
    mind_prompt_build(&pr, c->reply_lang, NULL, events);
    chat_message_t msgs[12];
    size_t nmsg = mind_dialog_messages(msgs, 12);
    chat_request_t req = { .system = pr.blocks, .system_count = pr.count, .messages = msgs, .message_count = nmsg,
                           .effort = CHAT_EFFORT_LOW, .max_tokens = 400, .temperature = 0.8f };   // thinking tokens count against max_tokens
    tag_sink_t sink = { on_text, on_tag, on_segment_end };
    tag_parser_init(&c->tags, &sink, c);
    if (!c->chat) { provider_error_t e; provider_error_set(&e, PERR_OTHER, 0, 0, "no chat session"); fail(c, "chat", &e); return; }
    vchat->chat_send(c->chat, &req);
    tag_parser_finish(&c->tags);
    seg_boundary(c);
    if (atomic_load(&c->abort)) return;
    if (c->chat_failed && c->reply.len == 0) { fail(c, "chat", &c->chat_err); return; }
    mind_dialog_add("assistant", c->reply.s ? c->reply.s : "");

    // 3. wait for the TTS to voice every segment
    int64_t deadline = clock_us() + 20 * 1000000LL;
    while (atomic_load(&c->seg_flushed) < atomic_load(&c->seg_sent) && !c->tts_failed && !atomic_load(&c->abort) && clock_us() < deadline) {
        signal_wait(c->tts_flush_sig, 200);
    }
    if (atomic_load(&c->abort)) return;
    if (c->tts_failed) { fail(c, "tts", &c->tts_err); tts_renew(c); return; }
    if (atomic_load(&c->seg_flushed) < atomic_load(&c->seg_sent)) { provider_error_t e; provider_error_set(&e, PERR_TIMEOUT, 0, 0, "tts did not finish"); fail(c, "tts", &e); tts_renew(c); return; }
    c->t.t4_turn_done = clock_us();
    engine_event_t done = { .kind = ENGINE_EV_TURN_DONE, .u.done = { c->t, c->u_asr, c->u_chat, c->u_tts, c->reply.s ? c->reply.s : "", c->transcript, c->reply_lang, atomic_load(&c->seg_sent), c->tags_seen } };
    emit(c, &done);
}

// The three pre-warms run concurrently: each costs a TLS handshake (and the TTS a task_start round trip).
struct prewarm { cascade_t *c; lang_t lang; esp_err_t r_asr, r_chat, r_tts; signal_t done; atomic_int pending; };
static void pw_finish(struct prewarm *p) { if (atomic_fetch_sub(&p->pending, 1) == 1) signal_set(p->done); }
static void pw_asr(void *arg)
{
    struct prewarm *p = arg; cascade_t *c = p->c;
    asr_params_t ap = { .lang_hint = p->lang, .sample_rate = 16000 }; asr_events_t ae = { NULL, asr_final, asr_error };
    p->r_asr = provider_for(PROVIDER_ROLE_ASR)->asr_open(&ap, &ae, c, &c->asr);
    pw_finish(p);
}
static void pw_chat(void *arg)
{
    struct prewarm *p = arg; cascade_t *c = p->c;
    chat_events_t ce = { chat_delta, chat_done, chat_error };
    p->r_chat = provider_for(PROVIDER_ROLE_CHAT)->chat_open(&ce, c, &c->chat);
    pw_finish(p);
}
static void pw_tts(void *arg)
{
    struct prewarm *p = arg; cascade_t *c = p->c;
    tts_session_t *t = NULL;
    p->r_tts = tts_open_session(c, &t);
    mutex_lock(c->lock); c->tts = p->r_tts == ESP_OK ? t : NULL; c->tts_failed = false; mutex_unlock(c->lock);
    pw_finish(p);
}

static void open_sessions(cascade_t *c, lang_t lang)
{
    provider_error_t e;
    if (!provider_for(PROVIDER_ROLE_ASR) || !provider_for(PROVIDER_ROLE_CHAT) || !provider_for(PROVIDER_ROLE_TTS)) {
        provider_error_set(&e, PERR_OTHER, 0, 0, "vendor not assigned"); fail(c, "prewarm", &e); return;
    }
    if (!net_is_online()) { provider_error_set(&e, PERR_NETWORK, 0, 0, "offline"); fail(c, "prewarm", &e); return; }
    int64_t t0 = clock_us();
    static struct prewarm pw;            // one engine, one pre-warm at a time
    pw = (struct prewarm){ .c = c, .lang = lang, .r_asr = ESP_FAIL, .r_chat = ESP_FAIL, .r_tts = ESP_FAIL };
    if (!pw.done) pw.done = signal_create();
    atomic_store(&pw.pending, 3);
    task_spawn("gugu_pw_asr", pw_asr, &pw, 8192, 5, 1);
    task_spawn("gugu_pw_chat", pw_chat, &pw, 8192, 5, 1);
    task_spawn("gugu_pw_tts", pw_tts, &pw, 8192, 5, 1);
    while (atomic_load(&pw.pending) > 0 && clock_us() - t0 < 20 * 1000000LL) signal_wait(pw.done, 200);
    LOGI("prewarm: asr %s chat %s tts %s in %lld ms", esp_err_to_name(pw.r_asr), esp_err_to_name(pw.r_chat), esp_err_to_name(pw.r_tts), (long long)((clock_us() - t0) / 1000));
    if (pw.r_asr != ESP_OK || pw.r_chat != ESP_OK || pw.r_tts != ESP_OK) {
        provider_error_set(&e, PERR_NETWORK, 0, 0, pw.r_tts != ESP_OK ? "tts session failed" : pw.r_chat != ESP_OK ? "chat connect failed" : "asr open failed");
        fail(c, "prewarm", &e);
        return;
    }
    atomic_store(&c->in_call, true);
    engine_event_t ev = { .kind = ENGINE_EV_READY };
    emit(c, &ev);
}

static void close_sessions(cascade_t *c)
{
    const provider_vtable_t *vasr = provider_for(PROVIDER_ROLE_ASR), *vchat = provider_for(PROVIDER_ROLE_CHAT), *vtts = provider_for(PROVIDER_ROLE_TTS);
    if (c->asr) vasr->asr_close(c->asr);
    if (c->chat) vchat->chat_close(c->chat);
    mutex_lock(c->lock); tts_session_t *t = c->tts; c->tts = NULL; mutex_unlock(c->lock);
    if (t) vtts->tts_close(t);
    c->asr = NULL; c->chat = NULL;
    atomic_store(&c->in_call, false);
    mind_dialog_reset();
}

static void engine_task(void *arg)
{
    cascade_t *c = arg;
    struct msg m;
    for (;;) {
        if (!queue_recv(c->q, &m, TASK_WAIT_FOREVER)) continue;
        if (m.cmd == CMD_QUIT) break;
        atomic_store(&c->busy, true);
        atomic_store(&c->abort, false);
        if (m.cmd == CMD_START) { c->lang_hint = m.lang; c->reply_lang = m.lang; open_sessions(c, m.lang); }
        else if (m.cmd == CMD_TURN) { run_turn(c, m.events[0] ? m.events : NULL); if (atomic_load(&c->abort)) { engine_event_t ev = { .kind = ENGINE_EV_ABORTED }; emit(c, &ev); } }
        else if (m.cmd == CMD_END) close_sessions(c);
        atomic_store(&c->busy, false);
    }
}

// ---- ops ----
static esp_err_t cs_start_call(engine_t *e, lang_t lang, const char *where)
{
    cascade_t *c = e->impl;
    struct msg m = { .cmd = CMD_START, .lang = lang };
    snprintf(m.where, sizeof m.where, "%s", where ? where : "");
    return queue_send(c->q, &m, 100) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static esp_err_t cs_on_audio(engine_t *e, const int16_t *pcm, size_t n)
{
    cascade_t *c = e->impl;
    if (!c->asr) return ESP_ERR_INVALID_STATE;
    return provider_for(PROVIDER_ROLE_ASR)->asr_feed(c->asr, pcm, n);
}
static esp_err_t cs_on_release(engine_t *e, const char *events)
{
    cascade_t *c = e->impl;
    struct msg m = { .cmd = CMD_TURN };
    snprintf(m.events, sizeof m.events, "%s", events ? events : "");
    return queue_send(c->q, &m, 100) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static void cs_abort(engine_t *e)
{
    cascade_t *c = e->impl;
    if (!atomic_load(&c->busy)) return;
    atomic_store(&c->abort, true);
    if (c->chat) provider_for(PROVIDER_ROLE_CHAT)->chat_cancel(c->chat);
    mutex_lock(c->lock);
    if (c->tts) provider_for(PROVIDER_ROLE_TTS)->tts_cancel(c->tts);
    mutex_unlock(c->lock);
    signal_set(c->tts_flush_sig);
}
static void cs_end_call(engine_t *e)
{
    cascade_t *c = e->impl;
    cs_abort(e);
    struct msg m = { .cmd = CMD_END };
    queue_send(c->q, &m, 100);
}
static bool cs_busy(engine_t *e) { return atomic_load(&((cascade_t *)e->impl)->busy); }
static void cs_destroy(engine_t *e)
{
    cascade_t *c = e->impl;
    struct msg m = { .cmd = CMD_QUIT }; queue_send(c->q, &m, 100);
    strbuf_free(&c->reply); strbuf_free(&c->seg_text);
    for (int i = 0; i < 32; i++) free(c->seg_texts[i]);
    mem_release(c);
}

static const engine_ops_t CASCADE_OPS = { cs_start_call, cs_on_audio, cs_on_release, cs_abort, cs_end_call, cs_busy, cs_destroy };

engine_t *engine_cascade_create(engine_listener_t cb, void *user)
{
    engine_t *e = calloc(1, sizeof *e);
    cascade_t *c = mem_alloc_big(sizeof *c);
    if (!e || !c) { free(e); mem_release(c); return NULL; }
    e->ops = &CASCADE_OPS; e->cb = cb; e->user = user; e->impl = c;
    c->e = e;
    c->q = queue_create(sizeof(struct msg), 8);
    c->lock = mutex_create();
    c->tts_flush_sig = signal_create();
    strbuf_init(&c->reply); strbuf_init(&c->seg_text);
    if (!task_spawn("gugu_engine", engine_task, c, 12288, 5, 1)) { free(e); free(c); return NULL; }
    return e;
}
