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
    void (*on_trouble)(const char *why, void *user);   // a turn failed or timed out: the app shows it (vibration, head shake); NULL = just the no-signal sound
    void *trouble_user;
} call_config_t;

esp_err_t    call_init(engine_t *engine, const call_config_t *cfg);
void         call_engine_event(const engine_event_t *ev, void *user);   // pass as the engine's listener
void         call_key(call_key_t key, bool down, int held_ms);           // app maps board keys onto these
void         call_hangup(void);
call_state_t call_state(void);
const char  *call_state_name(call_state_t s);
typedef void (*call_state_cb)(call_state_t state, void *user);
void         call_set_state_listener(call_state_cb cb, void *user);
// Replay harness: dial, feed a 16 kHz WAV as the utterance, release, wait for the turn to finish.
esp_err_t    call_replay_wav(const char *path, uint32_t timeout_ms);
// Same, but the utterance is spoken and the key released while the line is still opening (the pre-roll path).
esp_err_t    call_replay_wav_early(const char *path, uint32_t timeout_ms);
