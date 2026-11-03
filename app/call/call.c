#define LOG_TAG "call"
#include "call/call.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/task.h"
#include "foundation/ringbuf.h"
#include "audio/audio.h"
#include "ui/ui.h"
#include "store/store.h"
#include "mind/mind.h"
#include "provider/provider.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>

#define MARKER_BASE 100
#define PREROLL_SECONDS 8

static engine_t *s_engine;
static call_config_t s_cfg;
static atomic_int s_state = CALL_IDLE;
static atomic_bool s_blue_down;
static int64_t s_call_t0, s_waiting_since, s_thinking_since;
static store_call_t s_log;
static ringbuf_t s_preroll; static uint8_t *s_preroll_mem;
static atomic_bool s_turn_active;
static atomic_uint s_captured;        // samples captured since LISTENING began
static int64_t s_t5_first_play;
static engine_timing_t s_last_timing;
static mind_tag_t s_seg_emotion[32];
static bool s_seg_marker_pending[32];
static atomic_uint s_voice_samples;  // PCM the engine delivered for the current turn: proof the answer had sound
static atomic_int s_turns_done, s_turns_failed, s_turns_empty;   // empty: the key was held but no words were heard
static call_state_cb s_state_cb; static void *s_state_user;

const char *call_state_name(call_state_t s)
{
    static const char *const N[] = { "idle", "connecting", "waiting", "listening", "thinking", "speaking", "ended" };
    return s <= CALL_ENDED ? N[s] : "?";
}

// The ring is on whenever the key is held and the microphone is being recorded: while the line is still
// opening (the audio goes to the pre-roll) as well as while listening.
// While the key is held Gugu listens with its face (head up, curious); the rim ring is off unless the parent
// turns it on (setting listen_ring), because a lit ring is more interface than a three-year-old needs.
static bool s_ring_setting_read, s_ring_on;
static void update_ring(void)
{
    call_state_t st = (call_state_t)atomic_load(&s_state);
    bool listening = atomic_load(&s_blue_down) && (st == CALL_CONNECTING || st == CALL_LISTENING);
    if (!s_ring_setting_read) { s_ring_on = store_setting_int("listen_ring", 0) != 0; s_ring_setting_read = true; }
    if (s_ring_on) ui_set_listening(listening);
    static bool was;
    if (listening && !was) { ui_set_emotion(UI_EMO_CURIOUS); ui_play_action(UI_ACT_NOD); }
    was = listening;
}

static void set_state(call_state_t s)
{
    call_state_t old = (call_state_t)atomic_exchange(&s_state, s);
    if (old != s) {
        LOGI("state %s -> %s", call_state_name(old), call_state_name(s));
        if (s == CALL_WAITING) s_waiting_since = clock_us();
        if (s == CALL_THINKING) s_thinking_since = clock_us();
        update_ring();
        if (s_state_cb) s_state_cb(s, s_state_user);
    }
}

call_state_t call_state(void) { return (call_state_t)atomic_load(&s_state); }
void call_set_state_listener(call_state_cb cb, void *user) { s_state_user = user; s_state_cb = cb; }

static ui_emotion_t emotion_of(mind_tag_t t)
{
    switch (t) {
    case TAG_HAPPY: return UI_EMO_HAPPY; case TAG_LAUGHING: return UI_EMO_LAUGHING; case TAG_SURPRISED: return UI_EMO_SURPRISED;
    case TAG_CURIOUS: return UI_EMO_CURIOUS; case TAG_SHY: return UI_EMO_SHY; case TAG_SAD: return UI_EMO_SAD;
    case TAG_SLEEPY: return UI_EMO_SLEEPY; case TAG_PUFF: return UI_EMO_PUFF; case TAG_WAVE: return UI_EMO_WAVE;
    case TAG_ANGRY: return UI_EMO_ANGRY; case TAG_SCARED: return UI_EMO_SCARED; case TAG_PROUD: return UI_EMO_PROUD; case TAG_DIZZY: return UI_EMO_DIZZY;
    default: return UI_EMO_NEUTRAL;
    }
}

// ---- microphone → engine ----
static void capture_cb(const int16_t *pcm, size_t n, void *user)
{
    (void)user;
    atomic_fetch_add(&s_captured, (unsigned)n);
    if (call_state() == CALL_CONNECTING) { ringbuf_write(&s_preroll, pcm, n * 2); return; }
    engine_on_audio(s_engine, pcm, n);
}

static void flush_preroll(void)
{
    int16_t buf[320];
    size_t got;
    while ((got = ringbuf_read(&s_preroll, buf, sizeof buf)) > 0) engine_on_audio(s_engine, buf, got / 2);
}

// The key is up and the utterance is complete: hand it to the engine.
static void start_turn(void)
{
    audio_capture_stats_t cs; audio_capture_stats(&cs);
    LOGI("release: %u ms captured, peak %d, rms %d", (unsigned)cs.ms, cs.peak, cs.rms);
    store_event("released: %u ms of sound, peak %d, rms %d → asking", (unsigned)cs.ms, cs.peak, cs.rms);
    atomic_store(&s_turn_active, true);
    atomic_store(&s_voice_samples, 0);
    set_state(CALL_THINKING);
    ui_set_emotion(UI_EMO_CURIOUS);
    audio_sfx_play("static");                                      // the walkie-talkie: over, the answer is on its way
    engine_on_release(s_engine, NULL);
}

// ---- speaker markers → face ----
static void marker_cb(int id, void *user)
{
    (void)user;
    int seg = id - MARKER_BASE;
    if (seg < 0 || seg >= 32) return;
    if (seg == 0) s_t5_first_play = clock_us();
    ui_set_emotion(emotion_of(s_seg_emotion[seg]));
    if (call_state() == CALL_THINKING) set_state(CALL_SPEAKING);
}

static void log_turn(const engine_event_t *ev)
{
    const engine_timing_t *t = &ev->u.done.timing;
    MEM_BIG_BSS static char line[1600], reply[700], tr[300];
    // escape minimal JSON (quotes/newlines) for the two text fields
    size_t o = 0; for (const char *p = ev->u.done.reply; *p && o < sizeof reply - 3; p++) { if (*p == '"' || *p == '\\') reply[o++] = '\\'; reply[o++] = *p == '\n' ? ' ' : *p; } reply[o] = 0;
    o = 0; for (const char *p = ev->u.done.transcript; *p && o < sizeof tr - 3; p++) { if (*p == '"' || *p == '\\') tr[o++] = '\\'; tr[o++] = *p == '\n' ? ' ' : *p; } tr[o] = 0;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(line, sizeof line,
             "{\"t\":\"%s\",\"lang\":\"%s\",\"transcript\":\"%s\",\"reply\":\"%s\",\"segments\":%d,\"tags\":%d,"
             "\"ms\":{\"asr\":%lld,\"first_token\":%lld,\"first_tts\":%lld,\"first_play\":%lld,\"done\":%lld},"
             "\"usage\":{\"asr_ms\":%u,\"in_tokens\":%u,\"out_tokens\":%u,\"cached\":%u,\"tts_chars\":%u}}",
             ts, lang_code(ev->u.done.lang), tr, reply, ev->u.done.segments, ev->u.done.tags,
             (long long)((t->t1_transcript - t->t0_release) / 1000), (long long)((t->t2_first_token - t->t0_release) / 1000),
             (long long)((t->t3_first_audio - t->t0_release) / 1000), (long long)(s_t5_first_play ? (s_t5_first_play - t->t0_release) / 1000 : -1),
             (long long)((t->t4_turn_done - t->t0_release) / 1000),
             (unsigned)ev->u.done.asr.audio_ms, (unsigned)ev->u.done.chat.in_tokens, (unsigned)ev->u.done.chat.out_tokens,
             (unsigned)ev->u.done.chat.cached_tokens, (unsigned)ev->u.done.tts.chars);
    store_call_turn(&s_log, line);
    float fen = provider_turn_cost_fen(&ev->u.done.asr, &ev->u.done.chat, &ev->u.done.tts);
    char ledger[200];
    snprintf(ledger, sizeof ledger, "{\"t\":\"%s\",\"call\":\"%s\",\"fen\":%.3f,\"asr_ms\":%u,\"in\":%u,\"cached\":%u,\"out\":%u,\"tts_chars\":%u}",
             ts, s_log.id, (double)fen, (unsigned)ev->u.done.asr.audio_ms, (unsigned)ev->u.done.chat.in_tokens, (unsigned)ev->u.done.chat.cached_tokens,
             (unsigned)ev->u.done.chat.out_tokens, (unsigned)ev->u.done.tts.chars);
    store_ledger_add(ledger);
    LOGI("turn: asr %lld ms, first token %lld ms, first tts %lld ms, first play %lld ms, done %lld ms | %s -> %s",
         (long long)((t->t1_transcript - t->t0_release) / 1000), (long long)((t->t2_first_token - t->t0_release) / 1000),
         (long long)((t->t3_first_audio - t->t0_release) / 1000), (long long)(s_t5_first_play ? (s_t5_first_play - t->t0_release) / 1000 : -1),
         (long long)((t->t4_turn_done - t->t0_release) / 1000), ev->u.done.transcript, ev->u.done.reply);
    s_last_timing = *t;
}

// A turn failed or the service is too slow: the child sees and feels it at once (the app's reaction), and the
// event log says why.
static void trouble(const char *stage, const char *why)
{
    store_event("trouble at %s: %s", stage, why);
    if (s_cfg.on_outcome) s_cfg.on_outcome(false, stage, why, s_cfg.outcome_user);
    else audio_sfx_play("nosignal");
}

// ---- engine events (engine task / vendor threads) ----
void call_engine_event(const engine_event_t *ev, void *user)
{
    (void)user;
    switch (ev->kind) {
    case ENGINE_EV_READY:
        store_event("line open after %lld ms (key %s)", (long long)((clock_us() - s_call_t0) / 1000), atomic_load(&s_blue_down) ? "held" : "released");
        if (call_state() == CALL_CONNECTING) {
            flush_preroll();
            if (atomic_load(&s_blue_down)) set_state(CALL_LISTENING);   // s_captured keeps the pre-roll count
            else if (atomic_load(&s_captured) >= AUDIO_SAMPLE_RATE / 5) start_turn();   // spoke and let go before the line was open
            else set_state(CALL_WAITING);
        }
        break;
    case ENGINE_EV_TRANSCRIPT:
        LOGI("heard (%s): %s", lang_code(ev->u.transcript.lang), ev->u.transcript.text);
        { char said[80]; snprintf(said, sizeof said, "%s", ev->u.transcript.text); mind_utf8_cut(said, 60); store_event("heard (%s): %s", lang_code(ev->u.transcript.lang), said); }
        break;
    case ENGINE_EV_TAG:
        switch (ev->u.tag) {
        case TAG_NOD: ui_play_action(UI_ACT_NOD); break;
        case TAG_WAVE: ui_play_action(UI_ACT_WAVE); break;
        case TAG_PUFF: ui_play_action(UI_ACT_PUFF); break;
        case TAG_COO: audio_sfx_play("coo"); ui_play_action(UI_ACT_COO); break;
        case TAG_WHISTLE: audio_sfx_play("whistle"); ui_play_action(UI_ACT_WHISTLE); break;
        case TAG_PECK: audio_sfx_play("tap"); ui_play_action(UI_ACT_PECK); break;
        case TAG_JUMP: audio_sfx_play("pop"); ui_play_action(UI_ACT_JUMP); break;
        default: break;
        }
        break;
    case ENGINE_EV_SEGMENT_START:
        if (ev->u.segment.index < 32) { s_seg_emotion[ev->u.segment.index] = ev->u.segment.emotion; s_seg_marker_pending[ev->u.segment.index] = true; }
        if (ev->u.segment.text) LOGI("segment %d [%s] %s", ev->u.segment.index, mind_tag_name(ev->u.segment.emotion), ev->u.segment.text);
        break;
    case ENGINE_EV_AUDIO:
        if (ev->u.audio.segment < 32 && s_seg_marker_pending[ev->u.audio.segment]) {
            s_seg_marker_pending[ev->u.audio.segment] = false;
            audio_play_marker(MARKER_BASE + ev->u.audio.segment);   // fires when this audio starts sounding
        }
        audio_play_pcm(ev->u.audio.pcm, ev->u.audio.samples);
        atomic_fetch_add(&s_voice_samples, (unsigned)ev->u.audio.samples);
        break;
    case ENGINE_EV_SEGMENT_END:
        break;
    case ENGINE_EV_TURN_DONE:
        log_turn(ev);
        if (s_cfg.on_outcome) s_cfg.on_outcome(true, "", "", s_cfg.outcome_user);   // the service answered: whatever went wrong before is over
        {
            unsigned voice_ms = atomic_exchange(&s_voice_samples, 0) * 1000u / AUDIO_SAMPLE_RATE;
            LOGI("voice delivered: %u ms for %u characters", voice_ms, (unsigned)ev->u.done.tts.chars);
            if (ev->u.done.tts.chars > 4 && voice_ms < ev->u.done.tts.chars * 60u) {   // speech runs ~150–300 ms a character: this much less means audio went missing
                LOGW("voice looks truncated: %u ms for %u characters", voice_ms, (unsigned)ev->u.done.tts.chars);
                store_event("voice looks truncated: %u ms for %u characters", voice_ms, (unsigned)ev->u.done.tts.chars);
            }
            store_event("answered: %d segment(s), %u ms of voice, first sound %lld ms after release, done %lld ms", ev->u.done.segments, voice_ms,
                        (long long)((ev->u.done.timing.t3_first_audio - ev->u.done.timing.t0_release) / 1000), (long long)((ev->u.done.timing.t4_turn_done - ev->u.done.timing.t0_release) / 1000));
        }
        atomic_fetch_add(&s_turns_done, 1);
        atomic_store(&s_turn_active, false);
        if (call_state() == CALL_THINKING) set_state(CALL_SPEAKING);   // audio still draining
        break;
    case ENGINE_EV_NOTHING_HEARD:                 // held the key, said nothing audible: a soft "coo?" and a head tilt
        LOGI("nothing heard");
        store_event("nothing heard: the recognizer found no words");
        atomic_fetch_add(&s_turns_empty, 1);
        atomic_store(&s_turn_active, false);
        audio_sfx_play("coo");
        ui_set_emotion(UI_EMO_CURIOUS);
        if (call_state() == CALL_THINKING) set_state(CALL_WAITING);
        break;
    case ENGINE_EV_ERROR:
        LOGW("engine error at %s: %s", ev->u.error.stage, ev->u.error.err.message);
        atomic_fetch_add(&s_turns_failed, 1);
        atomic_store(&s_turn_active, false);
        store_event("turn failed at %s: %s (http %d)", ev->u.error.stage, ev->u.error.err.message, ev->u.error.err.http_status);
        { char why[96]; snprintf(why, sizeof why, "%s: %.70s", ev->u.error.stage, ev->u.error.err.message); trouble(ev->u.error.stage, why); }
        if (call_state() == CALL_CONNECTING) call_hangup();
        else if (call_state() == CALL_THINKING || call_state() == CALL_SPEAKING) set_state(CALL_WAITING);
        break;
    case ENGINE_EV_ABORTED:
        atomic_store(&s_turn_active, false);
        break;
    }
}

// ---- keys ----
static void dial(void)
{
    store_call_begin(&s_log);
    s_call_t0 = clock_us();
    ringbuf_clear(&s_preroll);
    atomic_store(&s_captured, 0);
    s_t5_first_play = 0;
    set_state(CALL_CONNECTING);
    audio_sfx_play("beep");                                        // the walkie-talkie, from the first press on (no dial tone: 2026-11-04)
    audio_capture_start(capture_cb, NULL);
    engine_start_call(s_engine, s_cfg.lang_hint, s_cfg.where);
    ui_set_emotion(UI_EMO_CURIOUS);
}

void call_key(call_key_t key, bool down, int held_ms)
{
    (void)held_ms;
    if (key != CALL_KEY_BLUE) return;
    atomic_store(&s_blue_down, down);
    call_state_t st = call_state();
    if (down) {
        switch (st) {
        case CALL_IDLE: case CALL_ENDED: dial(); break;
        case CALL_CONNECTING: audio_capture_start(capture_cb, NULL); break;
        case CALL_SPEAKING: case CALL_THINKING:
            engine_abort(s_engine); audio_play_flush();
            /* fall through */
        case CALL_WAITING:
            audio_play_flush();                                    // a whistle from the yellow key in the open line stops: the child's turn
            s_t5_first_play = 0;
            atomic_store(&s_captured, 0);
            audio_capture_start(capture_cb, NULL);
            audio_sfx_play("beep");                                // the walkie-talkie: your turn
            set_state(CALL_LISTENING);
            break;
        default: break;
        }
    } else if (st == CALL_LISTENING) {
        audio_capture_stop();
        if (atomic_load(&s_captured) >= AUDIO_SAMPLE_RATE / 5) start_turn();
        else {                                                     // a tap shorter than 200 ms: nothing to send
            audio_capture_stats_t cs; audio_capture_stats(&cs);
            LOGI("release after %u ms of audio: too short, nothing sent", (unsigned)cs.ms);
            store_event("released too soon: %u ms of sound, nothing sent", (unsigned)cs.ms);
            set_state(CALL_WAITING);
        }
    } else if (st == CALL_CONNECTING) {
        audio_capture_stop();   // the utterance stays in the pre-roll; READY sends it (a bare tap just opens the line)
    }
    update_ring();
}

void call_set_lang(lang_t lang) { s_cfg.lang_hint = lang; }

void call_hangup(void)
{
    call_state_t st = call_state();
    if (st == CALL_IDLE) return;
    audio_capture_stop();
    engine_end_call(s_engine);
    audio_play_flush();
    char summary[96];
    snprintf(summary, sizeof summary, "{\"turns_ok\":%d,\"turns_failed\":%d,\"turns_empty\":%d,\"seconds\":%lld}", atomic_load(&s_turns_done),
             atomic_load(&s_turns_failed), atomic_load(&s_turns_empty), (long long)((clock_us() - s_call_t0) / 1000000));
    store_call_end(&s_log, summary);
    store_event("hung up: %s", summary);
    store_logs_flush();                                            // the call's log lines reach flash now, not in 30 s
    atomic_store(&s_turns_done, 0); atomic_store(&s_turns_failed, 0); atomic_store(&s_turns_empty, 0);
    ui_set_emotion(UI_EMO_NEUTRAL);
    set_state(CALL_IDLE);
}

// ---- housekeeping task ----
static void call_task(void *arg)
{
    (void)arg;
    for (;;) {
        clock_sleep_ms(40);
        call_state_t st = call_state();
        if (st == CALL_IDLE || st == CALL_ENDED) continue;
        ui_set_mouth(audio_play_level());
        int64_t now = clock_us();
        if (st == CALL_SPEAKING && audio_play_idle() && !atomic_load(&s_turn_active)) set_state(CALL_WAITING);
        if (st == CALL_WAITING && s_cfg.idle_timeout_ms && now - s_waiting_since > (int64_t)s_cfg.idle_timeout_ms * 1000) { LOGI("idle timeout"); call_hangup(); }
        if (s_cfg.max_call_ms && now - s_call_t0 > (int64_t)s_cfg.max_call_ms * 1000) { LOGI("call time cap"); call_hangup(); }
        if (st == CALL_CONNECTING && now - s_call_t0 > 25 * 1000000LL) { LOGW("connect timeout"); trouble("connect", "the line did not open"); call_hangup(); }
        uint32_t deadline = s_cfg.answer_deadline_ms ? s_cfg.answer_deadline_ms : 15000;
        if (st == CALL_THINKING && atomic_load(&s_turn_active) && now - s_thinking_since > (int64_t)deadline * 1000) {
            LOGW("no answer %u ms after release: giving up on the turn", (unsigned)deadline);
            engine_abort(s_engine);                 // the late reply, if any, is dropped by the engine's abort flag
            atomic_store(&s_turn_active, false);
            atomic_fetch_add(&s_turns_failed, 1);
            trouble("deadline", "no answer in time");
            set_state(CALL_WAITING);
        }
    }
}

esp_err_t call_init(engine_t *engine, const call_config_t *cfg)
{
    s_engine = engine; s_cfg = *cfg;
    s_preroll_mem = mem_alloc_big(AUDIO_SAMPLE_RATE * 2 * PREROLL_SECONDS);
    if (!s_preroll_mem) return ESP_ERR_NO_MEM;
    ringbuf_init(&s_preroll, s_preroll_mem, AUDIO_SAMPLE_RATE * 2 * PREROLL_SECONDS);
    audio_set_marker_cb(marker_cb, NULL);
    if (!task_spawn("gugu_call", call_task, NULL, 8192, 3, 0)) return ESP_FAIL;   // a late answer reports from here (event log, the app, the bubble): 4 KB left 680 B before that path ran
    LOGI("call ready (idle %u ms, cap %u ms)", (unsigned)cfg->idle_timeout_ms, (unsigned)cfg->max_call_ms);
    return ESP_OK;
}

static esp_err_t wait_turn(int64_t since, uint32_t timeout_ms, int done_before, int failed_before, int empty_before)
{
    while (clock_us() - since < (int64_t)timeout_ms * 1000) {
        clock_sleep_ms(50);
        if (atomic_load(&s_turns_failed) > failed_before) return ESP_FAIL;
        if (atomic_load(&s_turns_empty) > empty_before) return ESP_ERR_NOT_FOUND;   // nothing heard
        if (atomic_load(&s_turns_done) > done_before && call_state() == CALL_WAITING) return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t call_replay_wav(const char *path, uint32_t timeout_ms)
{
    int64_t t0 = clock_us();
    if (call_state() == CALL_IDLE) {
        atomic_store(&s_captured, 0);
        call_key(CALL_KEY_BLUE, true, 0);
        clock_sleep_ms(50);
        call_key(CALL_KEY_BLUE, false, 50);
        // wait until the call is open and idle (a very fast vendor may already have run a turn)
        while (call_state() != CALL_WAITING && call_state() != CALL_IDLE && clock_us() - t0 < 30 * 1000000LL) clock_sleep_ms(50);
        if (call_state() != CALL_WAITING) { LOGE("replay: call did not connect (%s)", call_state_name(call_state())); return ESP_FAIL; }
    }
    int done_before = atomic_load(&s_turns_done), failed_before = atomic_load(&s_turns_failed), empty_before = atomic_load(&s_turns_empty);
    int64_t ta = clock_us();
    call_key(CALL_KEY_BLUE, true, 0);
    int64_t tb = clock_us();
    esp_err_t err = audio_capture_inject_wav(path, false);
    int64_t tc = clock_us();
    call_key(CALL_KEY_BLUE, false, 1000);
    int64_t t1 = clock_us();
    LOGI("replay: press %lld ms, inject %lld ms, release %lld ms", (long long)((tb - ta) / 1000), (long long)((tc - tb) / 1000), (long long)((t1 - tc) / 1000));
    if (err != ESP_OK) return err;
    return wait_turn(t1, timeout_ms, done_before, failed_before, empty_before);
}

esp_err_t call_replay_wav_early(const char *path, uint32_t timeout_ms)
{
    if (call_state() != CALL_IDLE) return ESP_ERR_INVALID_STATE;
    int done_before = atomic_load(&s_turns_done), failed_before = atomic_load(&s_turns_failed), empty_before = atomic_load(&s_turns_empty);
    call_key(CALL_KEY_BLUE, true, 0);                 // dial, speak into the pre-roll and let go while the line is still opening
    esp_err_t err = audio_capture_inject_wav(path, false);
    call_key(CALL_KEY_BLUE, false, 500);
    LOGI("replay early: released while %s", call_state_name(call_state()));
    if (err != ESP_OK) return err;
    return wait_turn(clock_us(), timeout_ms, done_before, failed_before, empty_before);
}
