// engine.h — one turn of conversation behind one interface: the cascade (ASR → LLM → tags → TTS) now,
// speech-to-speech later. The engine owns its task; it knows vendors and the mind, nothing about
// audio devices or screens — those listen to its events (CLAUDE.md rule 4).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "provider/provider.h"
#include "mind/mind.h"

typedef struct {
    int64_t t0_release;        // the child let go of the key
    int64_t t1_transcript;     // ASR final text available
    int64_t t2_first_token;    // first text delta from the model
    int64_t t3_first_audio;    // first PCM from the TTS
    int64_t t4_turn_done;      // last audio of the turn handed over
    int64_t t_chat_sent;       // the prompt was built and the request went to the model (between t1 and t2)
    int64_t t_first_segment;   // the first sentence was complete and handed to the voice (between t2 and t3)
} engine_timing_t;

typedef enum {
    ENGINE_EV_READY,           // start_call finished pre-warming (or failed: see error)
    ENGINE_EV_TRANSCRIPT,      // u.transcript
    ENGINE_EV_TAG,             // u.tag — in stream order, before the text it decorates
    ENGINE_EV_SEGMENT_START,   // u.segment — the TTS began a segment; carries the emotion to show with it
    ENGINE_EV_AUDIO,           // u.audio — PCM for the current segment
    ENGINE_EV_SEGMENT_END,     // u.segment
    ENGINE_EV_TURN_DONE,       // u.done
    ENGINE_EV_NOTHING_HEARD,   // the key was held but the ASR heard no words; nothing was sent to the model
    ENGINE_EV_ERROR,           // u.error — the turn (or the pre-warm) failed; the call decides what to say
    ENGINE_EV_ABORTED,         // the turn was cancelled
    ENGINE_EV_STORY,           // u.story — a story began, a part ended on a question for the child, or the story is over
} engine_event_kind_t;

// A story is told in parts, each its own request to the model. Every part but the last ends on a question; the call
// then waits for the child: an answer is the next turn (it goes into the next part), no answer is engine_story_next().
typedef enum { ENGINE_STORY_BEGIN, ENGINE_STORY_ASK, ENGINE_STORY_END } engine_story_phase_t;

typedef struct {
    engine_event_kind_t kind;
    union {
        struct { const char *text; lang_t lang; } transcript;
        mind_tag_t tag;
        struct { int index; mind_tag_t emotion; const char *text; } segment;
        struct { const int16_t *pcm; size_t samples; int segment; } audio;
        struct { engine_timing_t timing; provider_usage_t asr, chat, tts; const char *reply; const char *transcript; lang_t lang; int segments; int tags; } done;
        struct { provider_error_t err; const char *stage; } error;   // stage: "prewarm" | "asr" | "chat" | "tts"
        struct { engine_story_phase_t phase; int part, parts; } story;
    } u;
} engine_event_t;

typedef void (*engine_listener_t)(const engine_event_t *ev, void *user);

typedef struct engine engine_t;

engine_t *engine_cascade_create(engine_listener_t cb, void *user);
engine_t *engine_s2s_create(engine_listener_t cb, void *user);         // placeholder: returns NULL until implemented
void      engine_destroy(engine_t *e);

// Per call: pre-warm connections and open the TTS session; READY (or ERROR) follows asynchronously.
esp_err_t engine_start_call(engine_t *e, lang_t lang_hint, const char *where);
// During hold: microphone frames.
esp_err_t engine_on_audio(engine_t *e, const int16_t *pcm, size_t samples);
// On release: run the turn asynchronously; events follow. `events` is a sentence about what just happened (may be NULL).
esp_err_t engine_on_release(engine_t *e, const char *events);
// Cancel whatever is in flight (new key press, hang-up).
void      engine_abort(engine_t *e);
void      engine_end_call(engine_t *e);
bool      engine_busy(engine_t *e);

// Stories. A turn whose words ask for one starts it (mind_story_intent); so does engine_story_start (the shelf, the
// bedtime bubble: `request` stands in for the child's words). No answer came after a part: engine_story_next. The child
// broke in while it was told: engine_story_pause instead of engine_abort, with the segment that was sounding (the index
// of ENGINE_EV_SEGMENT_START, -1 none yet); the next turn answers the child and then goes on from that sentence.
esp_err_t engine_story_start(engine_t *e, const char *request, bool bedtime);
esp_err_t engine_story_next(engine_t *e);
void      engine_story_pause(engine_t *e, int sounding_segment);
bool      engine_story_on(engine_t *e);
// A part is longer than the playback buffer: the voice is fed only while less than this much audio waits to be heard.
// app.c hands in the speaker's queue (audio_play_queued_ms); without it the voice is fed as fast as it can speak.
void      engine_set_backlog_probe(engine_t *e, uint32_t (*queued_ms)(void));
