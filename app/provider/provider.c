// provider.c — registry, role assignment, credentials and the helpers vendor files share.
// Preset JSON loading lives in provider_preset.c.
#define LOG_TAG "provider"
#include "provider/provider.h"
#include "provider_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "store/store.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define MAX_VENDORS 10
static const provider_vtable_t *s_vendors[MAX_VENDORS];
static size_t s_vendor_count;
static const provider_vtable_t *s_role[5];   // index = bit position of provider_role_t
MEM_BIG_BSS static char s_cred[MAX_VENDORS][4][256];   // [vendor][field] values (keys can exceed 128 chars), never logged
static char s_cred_name[MAX_VENDORS][4][24];
static const char *const ROLE_NAME[5] = { "asr", "chat", "tts", "s2s", "image" };

const char *lang_code(lang_t l)
{
    switch (l) { case LANG_EN: return "en"; case LANG_ZH: return "zh"; case LANG_YUE: return "yue"; default: return ""; }
}

lang_t lang_from_code(const char *c)
{
    if (!c || !*c) return LANG_UNKNOWN;
    if (!strncasecmp(c, "yue", 3) || !strcasecmp(c, "zh-HK") || !strcasecmp(c, "zh-yue") || strcasestr(c, "Yue") || strcasestr(c, "Cantonese")) return LANG_YUE;
    if (!strncasecmp(c, "zh", 2) || strcasestr(c, "Chinese") || !strcasecmp(c, "cmn")) return LANG_ZH;
    if (!strncasecmp(c, "en", 2) || strcasestr(c, "English")) return LANG_EN;
    return LANG_UNKNOWN;
}

void provider_error_set(provider_error_t *e, provider_error_kind_t kind, int http, int api, const char *msg)
{
    e->kind = kind; e->http_status = http; e->api_code = api;
    snprintf(e->message, sizeof e->message, "%s", msg ? msg : "");
}

provider_error_kind_t provider_classify_http(int http_status)
{
    if (http_status == 0) return PERR_NETWORK;
    if (http_status == 401 || http_status == 403) return PERR_AUTH;
    if (http_status == 429 || http_status == 402) return PERR_QUOTA;
    if (http_status == 408 || http_status == 504) return PERR_TIMEOUT;
    if (http_status >= 200 && http_status < 300) return PERR_NONE;
    return PERR_OTHER;
}

static int vendor_index(const char *id)
{
    for (size_t i = 0; i < s_vendor_count; i++) if (!strcmp(s_vendors[i]->id, id)) return (int)i;
    return -1;
}

void provider_register(const provider_vtable_t *v)
{
    if (s_vendor_count < MAX_VENDORS) s_vendors[s_vendor_count++] = v;
}

static int role_slot(provider_role_t role)
{
    for (int i = 0; i < 5; i++) if (role == (1u << i)) return i;
    return -1;
}

const provider_vtable_t *provider_for(provider_role_t role)
{
    int s = role_slot(role);
    return s < 0 ? NULL : s_role[s];
}

esp_err_t provider_assign(provider_role_t role, const char *vendor_id)
{
    int s = role_slot(role), v = vendor_index(vendor_id);
    if (s < 0 || v < 0) return ESP_ERR_NOT_FOUND;
    if (!(s_vendors[v]->roles & role)) return ESP_ERR_NOT_SUPPORTED;
    s_role[s] = s_vendors[v];
    return ESP_OK;
}

const char *provider_credential(const char *vendor_id, const char *field)
{
    int v = vendor_index(vendor_id);
    if (v < 0) return NULL;
    for (int f = 0; f < 4; f++) if (!strcmp(s_cred_name[v][f], field)) return s_cred[v][f][0] ? s_cred[v][f] : NULL;
    return NULL;
}

esp_err_t provider_set_credential(const char *vendor_id, const char *field, const char *value)
{
    int v = vendor_index(vendor_id);
    if (v < 0) return ESP_ERR_NOT_FOUND;
    for (int f = 0; f < 4; f++) {
        if (!strcmp(s_cred_name[v][f], field) || s_cred_name[v][f][0] == '\0') {
            bool changed = strcmp(s_cred[v][f], value ? value : "") != 0;
            snprintf(s_cred_name[v][f], sizeof s_cred_name[v][f], "%s", field);
            snprintf(s_cred[v][f], sizeof s_cred[v][f], "%s", value ? value : "");
            char masked[8];
            if (changed) LOGI("credential %s.%s %s (%s)", vendor_id, field, value && *value ? "set" : "cleared", log_mask(value, masked));
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

const char *provider_region(const char *vendor_id)
{
    const provider_preset_t *p = provider_preset_get(vendor_id);
    return p && p->region[0] ? p->region : "domestic";
}

float provider_image_cost_fen(void)
{
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_IMAGE);
    const provider_preset_t *p = v ? provider_preset_get(v->id) : NULL;
    return p ? p->price_image * 100.0f : 0.0f;
}

float provider_turn_cost_fen(const provider_usage_t *asr, const provider_usage_t *chat, const provider_usage_t *tts)
{
    float fen = 0;
    const provider_vtable_t *v;
    const provider_preset_t *p;
    if ((v = provider_for(PROVIDER_ROLE_ASR)) && (p = provider_preset_get(v->id)) && asr)
        fen += p->price_asr_hour * 100.0f * (float)asr->audio_ms / 3600000.0f;
    if ((v = provider_for(PROVIDER_ROLE_CHAT)) && (p = provider_preset_get(v->id)) && chat) {
        uint32_t uncached = chat->in_tokens > chat->cached_tokens ? chat->in_tokens - chat->cached_tokens : 0;
        fen += 100.0f * ((float)uncached * p->price_in_m + (float)chat->cached_tokens * p->price_cache_m + (float)chat->out_tokens * p->price_out_m) / 1e6f;
    }
    if ((v = provider_for(PROVIDER_ROLE_TTS)) && (p = provider_preset_get(v->id)) && tts)
        fen += p->price_tts_10k * 100.0f * (float)tts->chars * 2.0f / 10000.0f;   // MiniMax bills 2 characters per CJK character
    return fen;
}

// Vendor registration list (one line per vendor file).
extern const provider_vtable_t provider_minimax;
extern const provider_vtable_t provider_qwen;
extern const provider_vtable_t provider_qwen_realtime;
extern const provider_vtable_t provider_volc;
extern const provider_vtable_t provider_fake;

static const char *default_vendor(provider_role_t role)   // the lowest-order preset whose vendor plays the role
{
    const provider_preset_t *best = NULL;
    for (size_t i = 0; provider_preset_at(i); i++) {
        const provider_preset_t *p = provider_preset_at(i);
        int v = vendor_index(p->id);
        if (v < 0 || !(s_vendors[v]->roles & role)) continue;
        if (!best || p->order < best->order) best = p;
    }
    return best ? best->id : NULL;
}

void provider_configure(void)
{
    bool reload = false;                                   // regions first: they decide the URLs
    for (size_t i = 0; provider_preset_at(i); i++) {
        char key[48]; snprintf(key, sizeof key, "region_%s", provider_preset_at(i)->id);
        if (provider_preset_pick_region(provider_preset_at(i)->id, store_setting_str(key, ""))) reload = true;
    }
    if (reload) provider_preset_reload();
    for (int s = 0; s < 5; s++) {
        if (s == 3) continue;                              // speech-to-speech is not the family's choice (yet)
        provider_role_t role = (provider_role_t)(1u << s);
        char key[24]; snprintf(key, sizeof key, "provider_%s", ROLE_NAME[s]);
        const char *want = store_setting_str(key, ""), *def = default_vendor(role);
        if (*want && provider_assign(role, want) == ESP_OK) continue;
        if (*want) LOGW("%s: %s cannot play it, using %s", key, want, def ? def : "none");
        if (def) provider_assign(role, def);
    }
    for (size_t i = 0; provider_preset_at(i); i++) {
        provider_preset_t *p = provider_preset_at(i);
        for (int c = 0; c < p->cred_n; c++) {              // keys from the secrets the preset names
            char v[256];
            if (store_secret_get(p->cred_store[c], v, sizeof v) != ESP_OK) v[0] = '\0';
            if (vendor_index(p->id) >= 0) provider_set_credential(p->id, p->cred_key[c], v);
        }
        static const char *const L[3] = { "en", "zh", "yue" };
        for (int l = 0; l < 3; l++) {                      // the family's voice per language
            char key[64]; snprintf(key, sizeof key, "voice_%s_%s", p->id, L[l]);
            snprintf(p->voice_pick[l], sizeof p->voice_pick[l], "%s", store_setting_str(key, ""));
        }
    }
    LOGI("configured: asr %s, chat %s, tts %s, image %s", s_role[0] ? s_role[0]->id : "-", s_role[1] ? s_role[1]->id : "-",
         s_role[2] ? s_role[2]->id : "-", s_role[4] ? s_role[4]->id : "-");
}

bool provider_role_ready(provider_role_t role, char *why, size_t cap)
{
    int s = role_slot(role);
    const provider_vtable_t *v = provider_for(role);
    if (s < 0 || !v) { snprintf(why, cap, "no %s vendor", s < 0 ? "?" : ROLE_NAME[s]); return false; }
    const provider_preset_t *p = provider_preset_get(v->id);
    if (!p) return true;                                   // no preset (the fake): nothing to sign with
    const char *field = p->role_cred[s][0] ? p->role_cred[s] : p->cred_n ? p->cred_key[0] : NULL;
    if (!field) return true;
    const char *k = provider_credential(v->id, field);
    if (k && *k) return true;
    snprintf(why, cap, "no %s key for %s (%s)", ROLE_NAME[s], v->id, field);
    return false;
}

esp_err_t provider_init(const char *preset_dir)
{
    s_vendor_count = 0;
    provider_register(&provider_minimax);
    provider_register(&provider_qwen);
    provider_register(&provider_qwen_realtime);
    provider_register(&provider_volc);
    provider_register(&provider_fake);
    esp_err_t err = provider_preset_load_dir(preset_dir);
    if (err != ESP_OK) LOGW("presets: %s (%s)", preset_dir, esp_err_to_name(err));
    // Defaults: the lowest-order preset that plays each role; the fake for the rest (provider_configure applies settings).
    for (int i = 0; i < 5; i++) {
        provider_role_t r = (provider_role_t)(1u << i);
        const char *def = default_vendor(r);
        s_role[i] = &provider_fake;
        if (def) provider_assign(r, def);
    }
    LOGI("%u vendors registered", (unsigned)s_vendor_count);
    return ESP_OK;
}
