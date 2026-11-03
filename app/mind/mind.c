// mind.c — façade: init and the per-call dialogue ring.
#define LOG_TAG "mind"
#include "mind/mind.h"
#include "mind_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "store/store.h"
#include <string.h>
#include <stdlib.h>

void mind_language_set_family(const lang_t langs[3]);

#define DIALOG_MAX 12
static struct { char role[10]; char *text; } s_dialog[DIALOG_MAX];
static size_t s_dialog_n;

esp_err_t mind_init(const mind_config_t *cfg)
{
    esp_err_t err = mind_prompt_load(cfg->content_dir);
    mind_set_child(cfg->child_name, cfg->child_age_months);
    mind_language_set_family(cfg->family_langs);
    mind_memory_init(store_data_dir());
    mind_dialog_reset();
    LOGI("mind ready (%s)", err == ESP_OK ? "prompts loaded" : "prompts missing");
    return err;
}

void mind_dialog_reset(void)
{
    for (size_t i = 0; i < s_dialog_n; i++) free(s_dialog[i].text);
    s_dialog_n = 0;
}

void mind_dialog_add(const char *role, const char *text)
{
    if (s_dialog_n == DIALOG_MAX) {           // drop the oldest pair
        free(s_dialog[0].text); free(s_dialog[1].text);
        memmove(&s_dialog[0], &s_dialog[2], sizeof s_dialog[0] * (DIALOG_MAX - 2));
        s_dialog_n -= 2;
    }
    snprintf(s_dialog[s_dialog_n].role, sizeof s_dialog[0].role, "%s", role);
    s_dialog[s_dialog_n].text = strdup(text ? text : "");
    s_dialog_n++;
}

const char *mind_dialog_last_user(void)
{
    for (size_t i = s_dialog_n; i > 0; i--) if (!strcmp(s_dialog[i - 1].role, "user")) return s_dialog[i - 1].text;
    return NULL;
}

size_t mind_dialog_messages(chat_message_t *out, size_t cap)
{
    size_t n = s_dialog_n < cap ? s_dialog_n : cap;
    for (size_t i = 0; i < n; i++) { out[i].role = s_dialog[i].role; out[i].text = s_dialog[i].text; }
    return n;
}
