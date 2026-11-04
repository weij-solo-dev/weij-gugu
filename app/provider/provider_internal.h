// provider_internal.h — private to the provider module: presets and registration.
#pragma once
#include "provider/provider.h"

typedef struct {
    char id[24];
    int order;                       // the default vendor for a role is the lowest order that plays it
    char region[12];                 // the region in use: the family's pick (settings region_<id>) or the first listed
    char regions[3][12]; int region_n;
    char cred_key[4][24], cred_store[4][40]; int cred_n;   // each credential and the secret (NVS key) that holds it
    char role_cred[5][24];           // per role slot (asr, chat, tts, s2s, image): the credential that role signs with
    char voice_pick[3][64];          // per language: the family's voice (settings voice_<id>_<lang>), "" = the preset's
    char asr_url[128], asr_model[48];
    char chat_url[128], chat_model[48], chat_model_alt[48];
    char chat_max_field[28];         // the request's token limit field: "max_tokens" (default) or "max_completion_tokens"
    bool chat_temperature;           // false: never send temperature (reasoning models reject or ignore it)
    char tts_url[128], tts_url_http[128], tts_model[48];
    uint32_t tts_rate;               // the rate the vendor's PCM comes at when it cannot be asked for 16 kHz (24000), else 0
    char image_url[128], image_model[48];
    char lang_boost[3][160];         // per lang_t - 1 (en, zh, yue): the voice's language hint (MiniMax language_boost, an instruction, a style)
    char default_voice[64];
    char default_voice_lang[3][64];  // per language override
    char chat_extra[3][160];         // raw JSON members per chat_effort_t (low, medium, high)
    bool asr_stream, tts_bidi, s2s, image;
    float price_asr_hour, price_tts_10k, price_in_m, price_out_m, price_cache_m, price_image;   // CNY list prices
} provider_preset_t;

esp_err_t provider_preset_load_dir(const char *dir);
esp_err_t provider_preset_load_json(const char *json_text);    // one preset document
const provider_preset_t *provider_preset_get(const char *vendor_id);
provider_preset_t *provider_preset_at(size_t i);                 // NULL past the end
bool provider_preset_pick_region(const char *vendor_id, const char *region);   // true if it changed (reload to apply)
esp_err_t provider_preset_reload(void);                          // the directory loaded last, again
// The voice a TTS session speaks with: the caller's, then the family's pick, then the preset's for the language, then "*".
const char *provider_voice(const provider_preset_t *pr, lang_t lang, const char *explicit_voice);
void provider_register(const provider_vtable_t *v);

// ---- shared by vendor files ----
// A transcript's language when the vendor does not say: Cantonese particles, else CJK, else Latin letters.
lang_t provider_guess_lang(const char *text);
// The base64 string value of "field" (the first after the text `after`, if given) decoded into a new PSRAM buffer
// (mem_release it); NULL if absent. Scans the text: a picture's few hundred KB never become a JSON tree.
uint8_t *provider_json_b64(const char *json, const char *after, const char *field, size_t *n);

// The voice for vendors that speak over plain HTTP, one request per segment (provider_httptts.c): flushed segments
// queue, a worker task speaks them in order, so tts_text / tts_flush never block the engine. The vendor's speak()
// makes one request and hands the PCM back through http_tts_pcm(); abort() cuts its request short (cancel, close).
typedef struct http_tts http_tts_t;
typedef esp_err_t (*http_tts_speak_fn)(http_tts_t *t, const char *text, provider_error_t *err);
http_tts_t *http_tts_create(const tts_params_t *p, const tts_events_t *ev, void *user, http_tts_speak_fn speak, void (*abort)(void *vendor), void *vendor);
void      *http_tts_vendor(http_tts_t *t);
const tts_params_t *http_tts_params(http_tts_t *t);
bool       http_tts_stale(http_tts_t *t);                                      // the segment being spoken was cancelled
void       http_tts_pcm(http_tts_t *t, const uint8_t *bytes, size_t n, uint32_t rate);   // 16-bit LE mono; 24 kHz becomes 16 kHz
esp_err_t  http_tts_text(http_tts_t *t, const char *utf8, size_t n);
esp_err_t  http_tts_flush(http_tts_t *t);
esp_err_t  http_tts_cancel(http_tts_t *t);
void       http_tts_destroy(http_tts_t *t);

// Reply parsers of the vendors that answer in JSON documents (exposed for the unit tests; the vendor files use them).
bool     provider_openai_transcript(const char *json, char *text, size_t cap, lang_t *lang);   // /v1/audio/transcriptions
bool     provider_gemini_text(const char *json, char *out, size_t cap);   // an interaction's first output text
uint8_t *provider_gemini_image(const char *json, size_t *n);              // its first output image, decoded (mem_release it)
// One step.delta event of a streamed speech interaction: the PCM in it decoded into out (returns bytes; *rate set),
// or 0 with err filled when the event is an error. Other events: 0, err empty.
size_t   provider_gemini_tts_event(const char *event, const char *data, uint8_t *out, size_t cap, uint32_t *rate, char *err, size_t err_cap);

// Volcengine binary frame helpers (shared by its ASR and TTS; exposed for the unit test).
#include "foundation/strbuf.h"
typedef struct { int type, flags, ser; int event; const uint8_t *payload; size_t n; int32_t seq; uint32_t err_code; } volc_frame_t;
size_t volc_frame_build(strbuf_t *out, int type, int flags, int ser, int event, const char *session, int32_t seq, const void *payload, size_t n);
bool   volc_frame_parse(const uint8_t *d, size_t n, volc_frame_t *f);
