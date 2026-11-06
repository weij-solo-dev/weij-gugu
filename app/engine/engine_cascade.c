// engine_cascade.c — the push-to-talk turn: ASR on release → prompt → streamed LLM → tag parser →
// TTS per segment (flush per segment keeps the segment/emotion pairing exact) → PCM events.
// One task per engine runs the blocking vendor calls; TTS audio arrives on the vendor's own thread.
//
// Stories (2026-11-06, the owner: "小朋友喜欢听长故事"; engine.h): a story is told in parts, each its own request with
// the story's prompt (mind_story_prompt_build). A part runs to a minute and a half, three times the 30 s the speaker can
// queue, so its sentences are kept and handed to the voice only while the speaker has less than FEED_AHEAD_MS waiting
// (the backlog probe app.c hands in). Every sentence of the part is kept until the part is over: when the child breaks
// in (engine_story_pause), the next turn answers the child and then voices the part again from the sentence that was
// sounding; a part the model was still writing is finished by a go-on request.
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

enum cmd { CMD_START, CMD_TURN, CMD_END, CMD_QUIT, CMD_STORY, CMD_STORY_NEXT };
struct msg { enum cmd cmd; lang_t lang; char where[96]; char events[96]; int64_t at; char request[200]; bool bedtime; };   // at: when the key was let go (CMD_TURN)

#define STORY_PARTS    3
#define STORY_SEGS     96          // sentences a part may have
#define FEED_AHEAD_MS  5000        // a story's sentences go to the voice while less than this waits in the speaker
#define VOICE_AHEAD    2           // … and while the voice has fewer than this many sentences still to say

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
    // the story (engine.h): told in parts; between parts the call waits for the child
    int story_part, story_parts;          // story_part 0: no story
    bool story_bedtime, story_paused, story_finished;
    strbuf_t story_text;                  // every part as written, for the shelf
    char *part_seg[STORY_SEGS]; mind_tag_t part_emo[STORY_SEGS];
    int part_n, fed;                      // this part's sentences so far; the next one to hand to the voice
    int part_told;                        // sentences already in the dialogue and the story's text
    bool part_complete;                   // the model finished writing the part
    int seg_partidx[32];                  // which sentence of the part each voice segment is (-1: none, an answer)
    int resume_from;                      // after a pause: the sentence it broke into
    atomic_int pause_heard;               // engine_story_pause: the segment sounding when the child pressed
    atomic_bool pause_req;
    bool pacing;                          // sentences wait for the speaker instead of going to the voice at once
    uint32_t (*backlog_ms)(void);
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
static void send_segment(cascade_t *c, const char *text, mind_tag_t emotion, int part_idx)
{
    int idx = atomic_load(&c->seg_sent);
    if (idx == 0) c->t.t_first_segment = clock_us();
    c->seg_emotion[idx & 31] = emotion;
    c->seg_partidx[idx & 31] = part_idx;
    free(c->seg_texts[idx & 31]);
    c->seg_texts[idx & 31] = strdup(text);
    atomic_store(&c->seg_sent, idx + 1);
    if (idx == atomic_load(&c->seg_playing)) {    // nothing ahead of it: this segment sounds next
        engine_event_t st = { .kind = ENGINE_EV_SEGMENT_START, .u.segment = { idx, emotion, c->seg_texts[idx & 31] } };
        emit(c, &st);
    }
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_TTS);
    if (c->tts && !c->tts_failed) {
        v->tts_text(c->tts, c->seg_texts[idx & 31], strlen(c->seg_texts[idx & 31]));
        v->tts_flush(c->tts);
    }
}

// A story's sentences go to the voice no faster than the speaker plays them (see the top of the file).
static void story_feed(cascade_t *c)
{
    while (c->fed < c->part_n && !atomic_load(&c->abort) && !c->tts_failed) {
        if (atomic_load(&c->seg_sent) - atomic_load(&c->seg_flushed) >= VOICE_AHEAD) return;
        if (c->backlog_ms && c->backlog_ms() >= FEED_AHEAD_MS) return;
        int i = c->fed++;
        send_segment(c, c->part_seg[i], c->part_emo[i], i);
    }
}

static void seg_boundary(cascade_t *c)
{
    if (c->seg_text.len == 0) return;
    if (c->pacing) {                              // a story: kept, voiced at the speaker's pace
        if (c->part_n < STORY_SEGS) { c->part_seg[c->part_n] = strdup(c->seg_text.s); c->part_emo[c->part_n] = c->pending_emotion; c->part_n++; }
        strbuf_clear(&c->seg_text);
        story_feed(c);
        return;
    }
    send_segment(c, c->seg_text.s, c->pending_emotion, -1);
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

static void turn_reset(cascade_t *c, int64_t at)
{
    memset(&c->t, 0, sizeof c->t); c->t.t0_release = at ? at : clock_us();
    c->asr_failed = c->chat_failed = c->chat_done = false;
    strbuf_clear(&c->reply); strbuf_clear(&c->seg_text);
    atomic_store(&c->seg_sent, 0); atomic_store(&c->seg_flushed, 0); atomic_store(&c->seg_playing, 0); c->pending_emotion = TAG_NEUTRAL;
    memset(&c->u_asr, 0, sizeof c->u_asr); memset(&c->u_chat, 0, sizeof c->u_chat); memset(&c->u_tts, 0, sizeof c->u_tts);
    c->tags_seen = 0;
    for (int i = 0; i < 32; i++) c->seg_partidx[i] = -1;
    atomic_store(&c->pause_heard, -1); atomic_store(&c->pause_req, false);
    c->pacing = false;
}

static bool voice_ready(cascade_t *c)   // a voice to answer with: the last turn may have lost it
{
    if ((!c->tts || c->tts_failed) && !tts_renew(c)) { provider_error_t e; provider_error_set(&e, PERR_NETWORK, 0, 0, "tts session could not reopen"); fail(c, "tts", &e); return false; }
    return true;
}

static void emit_done(cascade_t *c)
{
    c->t.t4_turn_done = clock_us();
    engine_event_t done = { .kind = ENGINE_EV_TURN_DONE, .u.done = { c->t, c->u_asr, c->u_chat, c->u_tts, c->reply.s ? c->reply.s : "", c->transcript, c->reply_lang, atomic_load(&c->seg_sent), c->tags_seen } };
    emit(c, &done);
}

// One request to the model, its answer cut into sentences on the way (straight to the voice, or kept for a story).
static void chat_stream(cascade_t *c, const mind_prompt_t *pr, uint32_t max_tokens, float temperature)
{
    chat_message_t msgs[12];
    size_t nmsg = mind_dialog_messages(msgs, 12);
    chat_request_t req = { .system = pr->blocks, .system_count = pr->count, .messages = msgs, .message_count = nmsg,
                           .effort = CHAT_EFFORT_LOW, .max_tokens = max_tokens, .temperature = temperature };   // thinking tokens count against max_tokens
    tag_sink_t sink = { on_text, on_tag, on_segment_end };
    tag_parser_init(&c->tags, &sink, c);
    c->chat_done = c->chat_failed = false;
    if (!c->t.t_chat_sent) c->t.t_chat_sent = clock_us();
    provider_for(PROVIDER_ROLE_CHAT)->chat_send(c->chat, &req);
    tag_parser_finish(&c->tags);
    seg_boundary(c);
}

// ---- stories ----
static void story_begin(cascade_t *c, bool bedtime)
{
    c->story_part = 1; c->story_parts = STORY_PARTS; c->story_bedtime = bedtime; c->story_paused = false;
    strbuf_clear(&c->story_text);
    engine_event_t ev = { .kind = ENGINE_EV_STORY, .u.story = { ENGINE_STORY_BEGIN, 1, STORY_PARTS, bedtime } };
    emit(c, &ev);
    LOGI("story begins (%d parts%s)", STORY_PARTS, bedtime ? ", bedtime" : "");
}

static void part_clear(cascade_t *c)
{
    for (int i = 0; i < c->part_n; i++) { free(c->part_seg[i]); c->part_seg[i] = NULL; }
    c->part_n = c->fed = c->part_told = 0; c->part_complete = false; c->resume_from = 0;
}

// The part's sentences not yet in the dialogue (the next part goes on from them) and the story's text (the shelf).
static void part_record(cascade_t *c)
{
    if (c->part_told >= c->part_n) return;
    strbuf_t t; strbuf_init(&t);
    for (int i = c->part_told; i < c->part_n; i++) {
        strbuf_append(&t, c->part_seg[i], strlen(c->part_seg[i]));
        char tag[24]; snprintf(tag, sizeof tag, "[%s]", mind_tag_name(c->part_emo[i]));   // the shelf keeps each sentence's face
        strbuf_append(&c->story_text, tag, strlen(tag)); strbuf_append(&c->story_text, c->part_seg[i], strlen(c->part_seg[i]));
    }
    mind_dialog_add("assistant", t.s ? t.s : "");
    strbuf_free(&t);
    c->part_told = c->part_n;
}

static void story_end(cascade_t *c, bool keep)
{
    if (!c->story_part) return;
    part_record(c);
    if (keep && c->story_text.len) mind_story_save(c->reply_lang, c->story_text.s, NULL, 0);
    engine_event_t ev = { .kind = ENGINE_EV_STORY, .u.story = { ENGINE_STORY_END, c->story_part, c->story_parts, c->story_bedtime, c->story_finished } };
    LOGI("story over after part %d of %d", c->story_part, c->story_parts);
    part_clear(c);
    c->story_part = 0; c->story_paused = false; c->story_finished = false; c->pacing = false;
    strbuf_clear(&c->story_text);
    emit(c, &ev);
}

// The child broke in (or the turn was cancelled) while the part was told: it goes on later from the sentence that was
// sounding (that one again, from its start).
static void story_paused_at(cascade_t *c)
{
    int heard = atomic_load(&c->pause_heard), sent = atomic_load(&c->seg_sent);
    int from = c->resume_from;
    if (heard >= 0)
        for (int i = sent - 1; i >= 0 && i >= sent - 32; i--)
            if ((i & 31) == (heard & 31)) { if (c->seg_partidx[i & 31] >= 0) from = c->seg_partidx[i & 31]; break; }
    if (heard < 0 && !atomic_load(&c->pause_req)) from = c->fed > 0 ? c->fed - 1 : 0;   // cancelled, nothing known: the last one sent
    c->resume_from = from < c->part_n ? from : c->part_n;
    c->story_paused = true;
    part_record(c);                               // the story so far, so the answer to the child knows it
    LOGI("story paused in part %d at sentence %d of %d%s", c->story_part, c->resume_from, c->part_n, c->part_complete ? "" : " (part not finished)");
}

// Every kept sentence voiced (false: cut short — a pause, a failure — or the voice stalled).
static bool story_drain(cascade_t *c)
{
    c->pacing = true;
    int64_t last = clock_us(); int flushed = atomic_load(&c->seg_flushed);
    for (;;) {
        if (atomic_load(&c->abort) || c->tts_failed) return false;
        story_feed(c);
        if (c->fed >= c->part_n && atomic_load(&c->seg_flushed) >= atomic_load(&c->seg_sent)) return true;
        signal_wait(c->tts_flush_sig, 200);
        int f = atomic_load(&c->seg_flushed);
        bool speaker_full = c->backlog_ms && c->backlog_ms() >= FEED_AHEAD_MS;   // waiting for the speaker is not a stall
        if (f != flushed || speaker_full) { flushed = f; last = clock_us(); }
        else if (clock_us() - last > 20 * 1000000LL) {
            provider_error_t e; provider_error_set(&e, PERR_TIMEOUT, 0, 0, "tts did not finish"); c->tts_failed = true; c->tts_err = e;
            return false;
        }
    }
}

// The part is written and voiced: into the dialogue and the story; the turn is done; a question for the child, or the end.
static void part_done(cascade_t *c)
{
    part_record(c);
    emit_done(c);
    if (c->story_part < c->story_parts) {
        engine_event_t ev = { .kind = ENGINE_EV_STORY, .u.story = { ENGINE_STORY_ASK, c->story_part, c->story_parts, c->story_bedtime } };
        part_clear(c);
        emit(c, &ev);
    } else { c->story_finished = true; story_end(c, true); }
}

static void story_cut(cascade_t *c)   // a part did not get through: a pause keeps the story, a failure ends it
{
    if (atomic_load(&c->abort)) { story_paused_at(c); return; }
    if (c->tts_failed) { fail(c, "tts", &c->tts_err); tts_renew(c); }
    else if (c->chat_failed) fail(c, "chat", &c->chat_err);
    story_end(c, true);
}

// Write part c->story_part (go_on: finish the one the child broke into) and voice it at the speaker's pace.
static bool story_write(cascade_t *c, const char *answer, bool go_on)
{
    mind_prompt_t pr;
    if (mind_story_prompt_build(&pr, c->reply_lang, c->story_part, c->story_parts, answer, c->story_bedtime, go_on) != ESP_OK || !c->chat) {
        provider_error_t e; provider_error_set(&e, PERR_OTHER, 0, 0, "story prompt or chat missing"); c->chat_failed = true; c->chat_err = e; return false;
    }
    c->pacing = true;
    int before = c->part_n;
    chat_stream(c, &pr, 1500, 0.9f);
    c->part_complete = c->chat_done && !atomic_load(&c->abort);
    if (atomic_load(&c->abort)) return false;
    if (c->chat_failed && c->part_n == before) return false;
    return story_drain(c);
}

static void story_part(cascade_t *c, const char *answer)
{
    part_clear(c);
    LOGI("story part %d of %d%s", c->story_part, c->story_parts, answer ? ", the child answered" : "");
    if (!story_write(c, answer, false)) { story_cut(c); return; }
    part_done(c);
}

// After a pause: answer what the child said (a sentence or two), then the part again from the sentence it broke into.
static void story_resume(cascade_t *c, const char *said)
{
    c->story_paused = false;
    if (said) {
        mind_prompt_t pr;
        mind_prompt_build(&pr, c->reply_lang, NULL, "你正在给孩子讲故事，讲到一半，孩子插嘴说了上面那句话。先用一两句话回答他，最后说「我们接着讲吧」（用这一通电话的语言说），别的不要说。");
        c->pacing = false;
        chat_stream(c, &pr, 400, 0.8f);
        if (atomic_load(&c->abort)) { story_paused_at(c); return; }
        if (c->reply.len) mind_dialog_add("assistant", c->reply.s);
        LOGI("story: answered the child (%u segments): %s", (unsigned)atomic_load(&c->seg_sent), c->reply.s ? c->reply.s : "");
    }
    LOGI("story goes on: part %d from sentence %d of %d", c->story_part, c->resume_from, c->part_n);
    c->fed = c->resume_from;
    if (!story_drain(c)) { story_cut(c); return; }
    if (!c->part_complete) {                      // the model was still writing it: the rest
        mind_dialog_add("user", said ? "（接着讲）" : "（接着讲吧）");
        if (!story_write(c, NULL, true)) { story_cut(c); return; }
    }
    part_done(c);
}

// "再讲一遍": the newest story off the shelf, told again as it was (its faces too), no request to the model, no new copy.
static void story_retell(cascade_t *c, const char *tagged)
{
    story_end(c, true);
    c->story_part = 1; c->story_parts = 1; c->story_bedtime = false; c->story_paused = false;
    strbuf_clear(&c->story_text);
    engine_event_t ev = { .kind = ENGINE_EV_STORY, .u.story = { ENGINE_STORY_BEGIN, 1, 1, false } };
    emit(c, &ev);
    LOGI("story told again from the shelf");
    part_clear(c);
    c->pacing = true;
    tag_sink_t sink = { on_text, on_tag, on_segment_end };
    tag_parser_init(&c->tags, &sink, c);
    tag_parser_feed(&c->tags, tagged, strlen(tagged));
    tag_parser_finish(&c->tags);
    seg_boundary(c);
    c->part_complete = true;
    c->part_told = c->part_n;                     // already on the shelf
    if (c->reply.len) mind_dialog_add("assistant", c->reply.s);
    if (!story_drain(c)) { story_cut(c); return; }
    emit_done(c);
    story_end(c, false);
}

static void run_story_next(cascade_t *c)          // no answer came: the next part
{
    if (!c->story_part || c->story_paused) return;
    turn_reset(c, 0); c->transcript[0] = '\0';
    if (!voice_ready(c)) { story_end(c, true); return; }
    c->story_part++;
    mind_dialog_add("user", "（没有回答）");
    story_part(c, NULL);
}

static void run_story_start(cascade_t *c, const char *request, bool bedtime)   // the shelf, the bedtime bubble
{
    turn_reset(c, 0); c->transcript[0] = '\0';
    if (!voice_ready(c)) return;
    story_end(c, true);
    mind_dialog_add("user", request && *request ? request : "给我讲个故事吧");
    story_begin(c, bedtime);
    story_part(c, NULL);
}

static void run_turn(cascade_t *c, const char *events, int64_t released_at)
{
    const provider_vtable_t *vasr = provider_for(PROVIDER_ROLE_ASR);
    turn_reset(c, released_at);
    c->transcript[0] = '\0';

    // 0. a voice to answer with: the last turn may have lost it
    if (!voice_ready(c)) return;

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
    int64_t tp0 = clock_us();
    engine_event_t tev = { .kind = ENGINE_EV_TRANSCRIPT, .u.transcript = { c->transcript, c->reply_lang } };
    emit(c, &tev);
    const char *gugu_said = mind_dialog_last_assistant();
    story_intent_t si = mind_story_intent(c->transcript, gugu_said);
    mind_dialog_add("user", c->transcript);

    // 1b. stories: the last one again, a new one, the answer to a part's question, or what the child said when it broke in
    if (si == STORY_INTENT_AGAIN) {
        char *again = mind_story_newest(c->reply_lang);
        if (again) { story_retell(c, again); mem_release(again); return; }   // none on the shelf yet: the model answers
    }
    if (c->chat && si == STORY_INTENT_START) { story_end(c, true); story_begin(c, false); story_part(c, NULL); return; }
    if (c->story_part && si == STORY_INTENT_STOP) {
        story_end(c, true);
        events = "孩子说不想听这个故事了：好好地收个尾，说下次再讲给他听。";
    } else if (c->story_part && c->story_paused) { story_resume(c, si == STORY_INTENT_MORE ? NULL : c->transcript); return; }
    else if (c->story_part) { c->story_part++; story_part(c, si == STORY_INTENT_MORE ? NULL : c->transcript); return; }

    // 2. prompt + chat (deltas drive the tag parser, segments drive the TTS)
    int64_t tp1 = clock_us();
    mind_prompt_t pr;
    mind_prompt_build(&pr, c->reply_lang, NULL, events);
    LOGI("prompt: transcript handed on in %lld ms, built in %lld ms", (long long)((tp1 - tp0) / 1000), (long long)((clock_us() - tp1) / 1000));
    if (!c->chat) { provider_error_t e; provider_error_set(&e, PERR_OTHER, 0, 0, "no chat session"); fail(c, "chat", &e); return; }
    c->t.t_chat_sent = clock_us();
    chat_stream(c, &pr, 400, 0.8f);
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
    emit_done(c);
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
    story_end(c, true);                            // hung up in the middle: what was told stays on the shelf
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
        else if (m.cmd == CMD_TURN || m.cmd == CMD_STORY || m.cmd == CMD_STORY_NEXT) {
            if (m.cmd == CMD_TURN) run_turn(c, m.events[0] ? m.events : NULL, m.at);
            else if (m.cmd == CMD_STORY) run_story_start(c, m.request, m.bedtime);
            else run_story_next(c);
            if (atomic_load(&c->abort)) { engine_event_t ev = { .kind = ENGINE_EV_ABORTED }; emit(c, &ev); }
        }
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
    struct msg m = { .cmd = CMD_TURN, .at = clock_us() };    // the turn's clock starts here, not when the engine task gets to it
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
static esp_err_t cs_story_start(engine_t *e, const char *request, bool bedtime)
{
    cascade_t *c = e->impl;
    struct msg m = { .cmd = CMD_STORY, .bedtime = bedtime };
    snprintf(m.request, sizeof m.request, "%s", request ? request : "");
    return queue_send(c->q, &m, 100) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static esp_err_t cs_story_next(engine_t *e)
{
    cascade_t *c = e->impl;
    if (!c->story_part) return ESP_ERR_INVALID_STATE;
    struct msg m = { .cmd = CMD_STORY_NEXT };
    return queue_send(c->q, &m, 100) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static void cs_story_pause(engine_t *e, int sounding)
{
    cascade_t *c = e->impl;
    if (c->story_part) { atomic_store(&c->pause_heard, sounding); atomic_store(&c->pause_req, true); }
    cs_abort(e);
}
static bool cs_story_on(engine_t *e) { return ((cascade_t *)e->impl)->story_part > 0; }
static void cs_set_backlog_probe(engine_t *e, uint32_t (*queued_ms)(void)) { ((cascade_t *)e->impl)->backlog_ms = queued_ms; }
static void cs_destroy(engine_t *e)
{
    cascade_t *c = e->impl;
    struct msg m = { .cmd = CMD_QUIT }; queue_send(c->q, &m, 100);
    strbuf_free(&c->reply); strbuf_free(&c->seg_text); strbuf_free(&c->story_text);
    for (int i = 0; i < 32; i++) free(c->seg_texts[i]);
    for (int i = 0; i < c->part_n; i++) free(c->part_seg[i]);
    mem_release(c);
}

static const engine_ops_t CASCADE_OPS = { cs_start_call, cs_on_audio, cs_on_release, cs_abort, cs_end_call, cs_busy, cs_destroy,
                                          cs_story_start, cs_story_next, cs_story_pause, cs_story_on, cs_set_backlog_probe };

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
    strbuf_init(&c->reply); strbuf_init(&c->seg_text); strbuf_init(&c->story_text);
    for (int i = 0; i < 32; i++) c->seg_partidx[i] = -1;
    atomic_store(&c->pause_heard, -1);
    if (!task_spawn("gugu_engine", engine_task, c, 12288, 5, 1)) { free(e); free(c); return NULL; }
    return e;
}
