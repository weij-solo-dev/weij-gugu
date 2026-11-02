// provider_preset.c — reads content/providers/<id>.json into provider_preset_t (CLAUDE.md rule 6:
// endpoints, models, voices, language hints and credentials are data, never code).
#define LOG_TAG "provider"
#include "provider_internal.h"
#include "foundation/log.h"
#include "foundation/atomic_file.h"
#include "foundation/mem.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>

MEM_BIG_BSS static provider_preset_t s_presets[8];
static size_t s_count;

static void copy_str(char *dst, size_t cap, cJSON *v)
{
    const char *s = cJSON_GetStringValue(v);
    if (s) snprintf(dst, cap, "%s", s); else dst[0] = '\0';
}

static void copy_url(char *dst, size_t cap, cJSON *urls, const char *region)
{
    if (!cJSON_IsObject(urls)) { dst[0] = '\0'; return; }
    cJSON *v = cJSON_GetObjectItem(urls, region);
    if (!v) v = urls->child;    // first available region
    copy_str(dst, cap, v);
}

// Serialise an object's members without the outer braces, e.g. {"a":1,"b":{}} -> "a":1,"b":{}
static void members_json(char *dst, size_t cap, cJSON *obj)
{
    dst[0] = '\0';
    if (!cJSON_IsObject(obj)) return;
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) return;
    size_t n = strlen(s);
    if (n >= 2) { s[n - 1] = '\0'; snprintf(dst, cap, "%s", s + 1); }
    cJSON_free(s);
}

esp_err_t provider_preset_load_json(const char *json_text)
{
    cJSON *j = cJSON_Parse(json_text);
    if (!j) return ESP_ERR_INVALID_RESPONSE;
    char id[24]; copy_str(id, sizeof id, cJSON_GetObjectItem(j, "id"));
    if (!id[0]) { cJSON_Delete(j); return ESP_ERR_INVALID_RESPONSE; }
    provider_preset_t *p = NULL;
    for (size_t i = 0; i < s_count; i++) if (!strcmp(s_presets[i].id, id)) p = &s_presets[i];   // reload replaces
    bool fresh = p == NULL;
    if (fresh) { if (s_count >= 8) { cJSON_Delete(j); return ESP_ERR_NO_MEM; } p = &s_presets[s_count]; }
    memset(p, 0, sizeof *p);
    snprintf(p->id, sizeof p->id, "%s", id);
    snprintf(p->region, sizeof p->region, "domestic");
    cJSON *regions = cJSON_GetObjectItem(j, "regions");
    if (cJSON_IsArray(regions) && cJSON_GetArraySize(regions) > 0) copy_str(p->region, sizeof p->region, cJSON_GetArrayItem(regions, 0));

    cJSON *caps = cJSON_GetObjectItem(j, "capabilities");
    p->asr_stream = cJSON_IsTrue(cJSON_GetObjectItem(caps, "asr_stream"));
    p->tts_bidi = cJSON_IsTrue(cJSON_GetObjectItem(caps, "tts_bidi"));
    p->s2s = cJSON_IsTrue(cJSON_GetObjectItem(caps, "s2s"));
    p->image = cJSON_IsTrue(cJSON_GetObjectItem(caps, "image"));

    cJSON *asr = cJSON_GetObjectItem(j, "asr");
    copy_url(p->asr_url, sizeof p->asr_url, cJSON_GetObjectItem(asr, "url"), p->region);
    copy_str(p->asr_model, sizeof p->asr_model, cJSON_GetObjectItem(asr, "model"));

    cJSON *chat = cJSON_GetObjectItem(j, "chat");
    copy_url(p->chat_url, sizeof p->chat_url, cJSON_GetObjectItem(chat, "url"), p->region);
    copy_str(p->chat_model, sizeof p->chat_model, cJSON_GetObjectItem(chat, "model"));
    copy_str(p->chat_model_alt, sizeof p->chat_model_alt, cJSON_GetObjectItem(chat, "model_alt"));
    cJSON *extra = cJSON_GetObjectItem(chat, "extra_body");
    const char *levels[3] = { "low", "medium", "high" };
    for (int i = 0; i < 3; i++) members_json(p->chat_extra[i], sizeof p->chat_extra[i], cJSON_GetObjectItem(extra, levels[i]));

    cJSON *tts = cJSON_GetObjectItem(j, "tts");
    copy_url(p->tts_url, sizeof p->tts_url, cJSON_GetObjectItem(tts, "url"), p->region);
    copy_url(p->tts_url_http, sizeof p->tts_url_http, cJSON_GetObjectItem(tts, "url_http"), p->region);
    copy_str(p->tts_model, sizeof p->tts_model, cJSON_GetObjectItem(tts, "model"));
    cJSON *per = cJSON_GetObjectItem(tts, "per_language");
    cJSON *hint = per ? per->child : NULL;        // the first (only) hint parameter, e.g. language_boost
    const char *langs[3] = { "en", "zh", "yue" };
    for (int i = 0; i < 3; i++) copy_str(p->lang_boost[i], sizeof p->lang_boost[i], cJSON_GetObjectItem(hint, langs[i]));

    cJSON *img = cJSON_GetObjectItem(j, "image");
    copy_url(p->image_url, sizeof p->image_url, cJSON_GetObjectItem(img, "url"), p->region);
    copy_str(p->image_model, sizeof p->image_model, cJSON_GetObjectItem(img, "model"));

    cJSON *pr = cJSON_GetObjectItem(j, "prices_cny");
    p->price_asr_hour = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(pr, "asr_per_hour"));
    p->price_tts_10k = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(pr, "tts_per_10k_chars"));
    p->price_in_m = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(pr, "chat_in_per_m"));
    p->price_out_m = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(pr, "chat_out_per_m"));
    p->price_cache_m = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(pr, "chat_cache_read_per_m"));
    p->price_image = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(pr, "image_per_picture"));
    cJSON *dv = cJSON_GetObjectItem(j, "default_voice");
    copy_str(p->default_voice, sizeof p->default_voice, cJSON_GetObjectItem(dv, "*"));
    for (int i = 0; i < 3; i++) copy_str(p->default_voice_lang[i], sizeof p->default_voice_lang[i], cJSON_GetObjectItem(dv, langs[i]));

    cJSON_Delete(j);
    if (fresh) s_count++;
    LOGI("preset %s loaded (%s)", p->id, p->region);
    return ESP_OK;
}

esp_err_t provider_preset_load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return ESP_ERR_NOT_FOUND;
    struct dirent *e; size_t loaded = 0;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json")) continue;
        char path[192];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        char *text = file_read_all(path, NULL);
        if (!text) continue;
        if (provider_preset_load_json(text) == ESP_OK) loaded++;
        else LOGW("bad preset %s", e->d_name);
        mem_release(text);
    }
    closedir(d);
    return loaded ? ESP_OK : ESP_ERR_NOT_FOUND;
}

const provider_preset_t *provider_preset_get(const char *vendor_id)
{
    for (size_t i = 0; i < s_count; i++) if (!strcmp(s_presets[i].id, vendor_id)) return &s_presets[i];
    return NULL;
}
