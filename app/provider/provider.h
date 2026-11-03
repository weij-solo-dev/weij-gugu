// provider.h — the vendor protocol (CLAUDE.md rule 6). Five roles, one session shape per role, events
// through callbacks, errors with the vendor's own code kept apart from the HTTP status. Nothing
// outside this module names a vendor; presets in content/providers/<id>.json declare endpoints,
// models, voices, language hints and credentials.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// ---- roles ----
typedef enum {
    PROVIDER_ROLE_ASR   = 1 << 0,
    PROVIDER_ROLE_CHAT  = 1 << 1,
    PROVIDER_ROLE_TTS   = 1 << 2,
    PROVIDER_ROLE_S2S   = 1 << 3,
    PROVIDER_ROLE_IMAGE = 1 << 4,
} provider_role_t;

// ---- errors, usage, timing ----
typedef enum {
    PERR_NONE = 0, PERR_NETWORK, PERR_AUTH, PERR_QUOTA, PERR_CONTENT_REJECTED,
    PERR_VOICE_NOT_FOUND, PERR_TIMEOUT, PERR_PROTOCOL, PERR_CANCELLED, PERR_OTHER,
} provider_error_kind_t;

typedef struct {
    provider_error_kind_t kind;
    int http_status;        // 0 if the transport never answered
    int api_code;           // the vendor's own code (MiniMax base_resp.status_code …), 0 if none
    char message[96];       // vendor text, truncated; never contains a secret
} provider_error_t;

typedef struct {
    uint32_t audio_ms;      // ASR / TTS audio duration billed or produced
    uint32_t chars;         // TTS characters billed
    uint32_t in_tokens, out_tokens, cached_tokens;   // CHAT
    uint32_t images;        // IMAGE: pictures produced
} provider_usage_t;

// ---- language ----
typedef enum { LANG_UNKNOWN = 0, LANG_EN, LANG_ZH, LANG_YUE } lang_t;
const char *lang_code(lang_t l);          // "en" | "zh" | "yue" | ""
lang_t lang_from_code(const char *code);  // accepts "en", "zh", "zh-CN", "yue", "zh-HK", "Chinese,Yue" …

// ============================================================================
// ASR — one shape for upload-at-end vendors (MiniMax) and streaming vendors (Qwen realtime, Volc):
// feed PCM while the key is held, finish() on release, the final transcript arrives in on_final.
// ============================================================================
typedef struct asr_session asr_session_t;
typedef struct {
    lang_t lang_hint;           // LANG_UNKNOWN = let the vendor detect
    uint32_t sample_rate;       // 16000
} asr_params_t;
typedef struct {
    void (*on_partial)(const char *text, void *user);                                   // streaming vendors only
    void (*on_final)(const char *text, lang_t lang, const provider_usage_t *usage, void *user);
    void (*on_error)(const provider_error_t *err, void *user);
} asr_events_t;

// ============================================================================
// CHAT — streaming text deltas. The request carries ordered system blocks (stable first, so the
// vendor's prefix cache hits) and the dialogue turns; effort selects thinking depth where supported.
// ============================================================================
typedef struct chat_session chat_session_t;
typedef enum { CHAT_EFFORT_LOW = 0, CHAT_EFFORT_MEDIUM, CHAT_EFFORT_HIGH } chat_effort_t;
typedef struct {
    const char *text;
    bool cache;                 // ask for a cache breakpoint after this block (vendors that support it)
} chat_block_t;
typedef struct {
    const char *role;           // "user" | "assistant"
    const char *text;
} chat_message_t;
typedef struct {
    const chat_block_t *system; size_t system_count;
    const chat_message_t *messages; size_t message_count;
    chat_effort_t effort;
    uint32_t max_tokens;
    float temperature;          // <= 0 means vendor default
} chat_request_t;
typedef struct {
    void (*on_delta)(const char *utf8, size_t n, void *user);
    void (*on_done)(const char *stop_reason, const provider_usage_t *usage, void *user);
    void (*on_error)(const provider_error_t *err, void *user);
} chat_events_t;

// ============================================================================
// TTS — text in, 16 kHz mono PCM out. Bidirectional-stream vendors map directly; HTTP-stream vendors
// buffer per sentence inside their implementation. flush() pushes the tail without closing.
// ============================================================================
typedef struct tts_session tts_session_t;
typedef struct {
    const char *voice_id;       // NULL = preset default for the language
    lang_t lang;                // selects the vendor's language hint
    float speed;                // 1.0
    uint32_t sample_rate;       // 16000
} tts_params_t;
typedef struct {
    void (*on_sentence_start)(const char *text, void *user);     // text may be NULL if the vendor does not report it
    void (*on_audio)(const int16_t *pcm, size_t samples, void *user);
    void (*on_sentence_end)(void *user);
    void (*on_flushed)(const provider_usage_t *usage, void *user);  // all text sent before flush() has been voiced
    void (*on_error)(const provider_error_t *err, void *user);
} tts_events_t;

// ============================================================================
// S2S — speech in, speech out, both transcripts back. Optional; the slot for Qwen Omni realtime.
// ============================================================================
typedef struct s2s_session s2s_session_t;
typedef struct {
    const char *instructions;   // persona + state, re-sent per session
    const char *voice_id;
    lang_t lang;
} s2s_params_t;
typedef struct {
    void (*on_transcript_in)(const char *text, void *user);
    void (*on_transcript_out)(const char *text, bool final, void *user);
    void (*on_audio)(const int16_t *pcm, size_t samples, void *user);
    void (*on_done)(const provider_usage_t *usage, void *user);
    void (*on_error)(const provider_error_t *err, void *user);
} s2s_events_t;

// ============================================================================
// IMAGE — generate one picture and save it to a file (postcards, souvenirs).
// ============================================================================
typedef struct {
    const char *prompt;
    const char *aspect;         // "1:1"
    const char *out_path;       // where the JPEG goes
} image_params_t;

// ---- the vtable every vendor file fills in (NULL for roles it does not play) ----
typedef struct provider_vtable {
    const char *id;             // "minimax" | "qwen" | "qwen_realtime" | "volc" | "fake"
    uint32_t roles;             // provider_role_t bitmask

    esp_err_t (*asr_open)(const asr_params_t *p, const asr_events_t *ev, void *user, asr_session_t **out);
    esp_err_t (*asr_feed)(asr_session_t *s, const int16_t *pcm, size_t samples);
    esp_err_t (*asr_finish)(asr_session_t *s);
    void      (*asr_close)(asr_session_t *s);

    esp_err_t (*chat_open)(const chat_events_t *ev, void *user, chat_session_t **out);   // may pre-connect
    esp_err_t (*chat_send)(chat_session_t *s, const chat_request_t *req);
    void      (*chat_cancel)(chat_session_t *s);
    void      (*chat_close)(chat_session_t *s);

    esp_err_t (*tts_open)(const tts_params_t *p, const tts_events_t *ev, void *user, tts_session_t **out);
    esp_err_t (*tts_text)(tts_session_t *s, const char *utf8, size_t n);
    esp_err_t (*tts_flush)(tts_session_t *s);
    esp_err_t (*tts_cancel)(tts_session_t *s);
    void      (*tts_close)(tts_session_t *s);

    esp_err_t (*s2s_open)(const s2s_params_t *p, const s2s_events_t *ev, void *user, s2s_session_t **out);
    esp_err_t (*s2s_audio)(s2s_session_t *s, const int16_t *pcm, size_t samples);
    esp_err_t (*s2s_commit)(s2s_session_t *s);
    void      (*s2s_close)(s2s_session_t *s);

    esp_err_t (*image_generate)(const image_params_t *p, provider_usage_t *usage, provider_error_t *err);
} provider_vtable_t;

// ---- registry and configuration ----
// Load presets from content/providers/*.json and the role selection + credentials from settings/NVS.
esp_err_t provider_init(const char *preset_dir);
// The vendor currently assigned to a role (settings: asr=minimax, chat=minimax, …). NULL if none.
const provider_vtable_t *provider_for(provider_role_t role);
// Override the assignment at runtime (settings page, replay harness).
esp_err_t provider_assign(provider_role_t role, const char *vendor_id);
// Credentials and endpoints are read by vendor files through these; never logged.
const char *provider_credential(const char *vendor_id, const char *field);   // "api_key", "appid" …
esp_err_t provider_set_credential(const char *vendor_id, const char *field, const char *value);
const char *provider_region(const char *vendor_id);                          // "domestic" | "overseas"

// Cost of one turn's usage in CNY cents (list prices from the presets of the vendors assigned to each role).
float provider_turn_cost_fen(const provider_usage_t *asr, const provider_usage_t *chat, const provider_usage_t *tts);
float provider_image_cost_fen(void);   // list price of one picture from the vendor assigned to the IMAGE role

// Helpers shared by vendor files.
void provider_error_set(provider_error_t *e, provider_error_kind_t kind, int http, int api, const char *msg);
provider_error_kind_t provider_classify_http(int http_status);
