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
    char tts_url[128], tts_url_http[128], tts_model[48];
    char image_url[128], image_model[48];
    char lang_boost[3][24];          // per lang_t - 1 (en, zh, yue): the vendor's language hint value
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

// Volcengine binary frame helpers (shared by its ASR and TTS; exposed for the unit test).
#include "foundation/strbuf.h"
typedef struct { int type, flags, ser; int event; const uint8_t *payload; size_t n; int32_t seq; uint32_t err_code; } volc_frame_t;
size_t volc_frame_build(strbuf_t *out, int type, int flags, int ser, int event, const char *session, int32_t seq, const void *payload, size_t n);
bool   volc_frame_parse(const uint8_t *d, size_t n, volc_frame_t *f);
