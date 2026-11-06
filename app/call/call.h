// call.h — the phone call: push-to-talk state machine around one engine. Blue key down dials (or
// starts listening), up releases a turn; the call routes engine audio to the speaker with markers so
// the face changes exactly when a segment starts sounding; idle and total timeouts hang up.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "engine/engine.h"

typedef enum { CALL_KEY_BLUE = 0, CALL_KEY_YELLOW = 1 } call_key_t;   // mirrors board_key_t without depending on board

typedef enum { CALL_IDLE, CALL_CONNECTING, CALL_WAITING, CALL_LISTENING, CALL_THINKING, CALL_SPEAKING, CALL_ENDED } call_state_t;

typedef struct {
    uint32_t idle_timeout_ms;    // WAITING with nothing happening → hang up (home: ~10 s)
    uint32_t max_call_ms;        // parent's cap on one call
    lang_t lang_hint;            // LANG_UNKNOWN = let the ASR decide
    const char *where;           // block-C sentence for this call
    uint32_t answer_deadline_ms; // released and nothing heard back after this long: give up on the turn (0 = 15 s)
    // Each turn's outcome, for the app: ok, or the stage that failed ("asr", "chat", "tts", "prewarm", "connect" or
    // "deadline") and one line. The app shows a failure (the bubble) and clears it on the next answer. NULL = a failure
    // only plays the no-signal sound.
    void (*on_outcome)(bool ok, const char *stage, const char *why, void *user);
    void *outcome_user;
    void (*on_story_over)(bool bedtime, void *user);   // a story ended (told to the end, stopped, or hung up): the app's goodnight
    void *story_user;
} call_config_t;

esp_err_t    call_init(engine_t *engine, const call_config_t *cfg);
void         call_engine_event(const engine_event_t *ev, void *user);   // pass as the engine's listener
void         call_key(call_key_t key, bool down, int held_ms);           // app maps board keys onto these
void         call_hangup(void);
call_state_t call_state(void);
const char  *call_state_name(call_state_t s);
void         call_set_lang(lang_t lang);   // the interaction language for the next call (the pairing page changed it)
// A story without the child's words (the shelf, the bedtime bubble): the line opens if it is closed (no beep, no
// microphone) and Gugu starts telling. `request` stands in for what the child would have said.
esp_err_t    call_story(const char *request, bool bedtime);
typedef void (*call_state_cb)(call_state_t state, void *user);
void         call_set_state_listener(call_state_cb cb, void *user);
// Replay harness: dial, feed a 16 kHz WAV as the utterance, release, wait for the turn to finish.
esp_err_t    call_replay_wav(const char *path, uint32_t timeout_ms);
// Same, but the utterance is spoken and the key released while the line is still opening (the pre-roll path).
esp_err_t    call_replay_wav_early(const char *path, uint32_t timeout_ms);
