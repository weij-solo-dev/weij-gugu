// provider_preset.c — reads content/providers/<id>.json into provider_preset_t (CLAUDE.md rule 6:
// endpoints, models, voices, language hints and credentials are data, never code).
#define LOG_TAG "provider"
#include "provider_internal.h"
#include "foundation/log.h"
#include "foundation/atomic_file.h"
#include "foundation/mem.h"
#include "store/store.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>

MEM_BIG_BSS static provider_preset_t s_presets[10];
static size_t s_count;
static char s_dir[160];
static struct { char id[24], region[12]; } s_region_pick[10];   // the family's region per vendor, applied at (re)load

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
    if (fresh) { if (s_count >= 10) { cJSON_Delete(j); return ESP_ERR_NO_MEM; } p = &s_presets[s_count]; }
    memset(p, 0, sizeof *p);
    snprintf(p->id, sizeof p->id, "%s", id);
    cJSON *ord = cJSON_GetObjectItem(j, "order");
    p->order = cJSON_IsNumber(ord) ? (int)ord->valuedouble : 99;
    snprintf(p->region, sizeof p->region, "domestic");
    cJSON *regions = cJSON_GetObjectItem(j, "regions");
    for (cJSON *r = cJSON_IsArray(regions) ? regions->child : NULL; r && p->region_n < 3; r = r->next) copy_str(p->regions[p->region_n++], sizeof p->regions[0], r);
    if (p->region_n) snprintf(p->region, sizeof p->region, "%s", p->regions[0]);
    for (int i = 0; i < 10; i++) {                       // the family's pick, if this preset offers it
        if (strcmp(s_region_pick[i].id, id)) continue;
        for (int r = 0; r < p->region_n; r++) if (!strcmp(p->regions[r], s_region_pick[i].region)) snprintf(p->region, sizeof p->region, "%s", p->regions[r]);
    }
    cJSON *creds = cJSON_GetObjectItem(j, "credentials");
    for (cJSON *c = cJSON_IsArray(creds) ? creds->child : NULL; c && p->cred_n < 4; c = c->next) {
        char key[24]; copy_str(key, sizeof key, cJSON_GetObjectItem(c, "key"));
        snprintf(p->cred_key[p->cred_n], sizeof p->cred_key[0], "%s", key);
        copy_str(p->cred_store[p->cred_n], sizeof p->cred_store[0], cJSON_GetObjectItem(c, "store"));
        if (!p->cred_store[p->cred_n][0]) snprintf(p->cred_store[p->cred_n], sizeof p->cred_store[0], "%.15s_%.23s", id, key);   // default: <id>_<key>
        store_secret_declare(p->cred_store[p->cred_n]);   // the page, the export and the masks learn it exists
        p->cred_n++;
    }
    static const char *const ROLE_KEYS[5] = { "asr", "chat", "tts", "s2s", "image" };
    for (int r = 0; r < 5; r++) {                        // the credential a role signs with: its credential_map's (first) value
        cJSON *m = cJSON_GetObjectItem(cJSON_GetObjectItem(j, ROLE_KEYS[r]), "credential_map");
        if (cJSON_IsObject(m) && m->child) copy_str(p->role_cred[r], sizeof p->role_cred[r], m->child);
    }

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
    if (dir != s_dir) snprintf(s_dir, sizeof s_dir, "%s", dir);
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

provider_preset_t *provider_preset_at(size_t i) { return i < s_count ? &s_presets[i] : NULL; }

bool provider_preset_pick_region(const char *vendor_id, const char *region)
{
    int slot = -1;
    for (int i = 0; i < 10; i++) if (!strcmp(s_region_pick[i].id, vendor_id)) slot = i;
    if (slot < 0) for (int i = 0; i < 10 && slot < 0; i++) if (!s_region_pick[i].id[0]) slot = i;
    if (slot < 0 || !strcmp(s_region_pick[slot].region, region ? region : "")) return false;
    snprintf(s_region_pick[slot].id, sizeof s_region_pick[slot].id, "%s", vendor_id);
    snprintf(s_region_pick[slot].region, sizeof s_region_pick[slot].region, "%s", region ? region : "");
    return true;
}

esp_err_t provider_preset_reload(void) { return s_dir[0] ? provider_preset_load_dir(s_dir) : ESP_ERR_INVALID_STATE; }

const char *provider_voice(const provider_preset_t *pr, lang_t lang, const char *explicit_voice)
{
    if (explicit_voice && *explicit_voice) return explicit_voice;
    if (!pr) return "";
    int li = lang == LANG_EN ? 0 : lang == LANG_ZH ? 1 : lang == LANG_YUE ? 2 : -1;
    if (li >= 0 && pr->voice_pick[li][0]) return pr->voice_pick[li];
    if (li >= 0 && pr->default_voice_lang[li][0]) return pr->default_voice_lang[li];
    return pr->default_voice;
}
