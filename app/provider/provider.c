// provider.c — registry, role assignment, credentials and the helpers vendor files share.
// Preset JSON loading lives in provider_preset.c.
#define LOG_TAG "provider"
#include "provider/provider.h"
#include "provider_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

static const provider_vtable_t *s_vendors[8];
static size_t s_vendor_count;
static const provider_vtable_t *s_role[5];   // index = bit position of provider_role_t
MEM_BIG_BSS static char s_cred[8][4][256];   // [vendor][field] values (keys can exceed 128 chars), never logged
static char s_cred_name[8][4][16];

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
    if (s_vendor_count < 8) s_vendors[s_vendor_count++] = v;
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
            snprintf(s_cred_name[v][f], sizeof s_cred_name[v][f], "%s", field);
            snprintf(s_cred[v][f], sizeof s_cred[v][f], "%s", value ? value : "");
            char masked[8];
            LOGI("credential %s.%s set (%s)", vendor_id, field, log_mask(value, masked));
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
    // Defaults: MiniMax for every role it plays; the fake for the rest (replaced by settings later).
    for (int i = 0; i < 5; i++) {
        provider_role_t r = (provider_role_t)(1u << i);
        if (provider_minimax.roles & r) s_role[i] = &provider_minimax;
        else s_role[i] = &provider_fake;
    }
    LOGI("%u vendors registered", (unsigned)s_vendor_count);
    return ESP_OK;
}
