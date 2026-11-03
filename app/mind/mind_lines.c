// mind_lines.c — what Gugu says on its own, from the bubble: the self-introduction (content/prompts/hello.json), a
// question for the child (questions.json), and the request that retells one fact of the trip in the child's language
// (share.md). The files are small and read when needed: these lines come a few times a day.
#define LOG_TAG "mind"
#include "mind/mind.h"
#include "mind_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/atomic_file.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static char s_dir[160];

void mind_lines_init(const char *content_dir)
{
    snprintf(s_dir, sizeof s_dir, "%s", content_dir ? content_dir : "content");
}

static cJSON *load_json(const char *rel)
{
    char path[220]; snprintf(path, sizeof path, "%s/prompts/%s", s_dir, rel);
    char *s = file_read_all(path, NULL);
    if (!s) { LOGW("missing %s", path); return NULL; }
    cJSON *j = cJSON_Parse(s); mem_release(s);
    return j;
}

static const char *code(lang_t l) { return l == LANG_EN ? "en" : l == LANG_YUE ? "yue" : "zh"; }

esp_err_t mind_line_hello(lang_t lang, char *out, size_t cap)
{
    cJSON *j = load_json("hello.json");
    const char *tpl = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, code(lang))) : NULL;
    if (!tpl) { cJSON_Delete(j); return ESP_ERR_NOT_FOUND; }
    char name_comma[64] = "";
    if (mind_child_named()) snprintf(name_comma, sizeof name_comma, "%s%s", lang == LANG_EN ? ", " : "，", mind_child_name());
    const char *const pairs[] = { "name_comma", name_comma, NULL };
    mind_fill(out, cap, tpl, pairs);
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t mind_line_question(lang_t lang, unsigned pick, char *out, size_t cap)
{
    cJSON *j = load_json("questions.json");
    cJSON *list = j ? cJSON_GetObjectItem(j, code(lang)) : NULL;
    int n = cJSON_GetArraySize(list);
    if (n <= 0) { cJSON_Delete(j); return ESP_ERR_NOT_FOUND; }
    snprintf(out, cap, "%s", cJSON_GetStringValue(cJSON_GetArrayItem(list, (int)(pick % (unsigned)n))));
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t mind_share_request_build(lang_t lang, const char *where_phrase, const char *fact, char *system, size_t system_cap)
{
    char path[220]; snprintf(path, sizeof path, "%s/prompts/share.md", s_dir);
    char *tpl = file_read_all(path, NULL);
    if (!tpl) { LOGW("missing %s", path); return ESP_ERR_NOT_FOUND; }
    const char *lang_name = lang == LANG_EN ? "English" : lang == LANG_YUE ? "粤语（广东话口语，用粤语字）" : "普通话（简体中文）";
    const char *const pairs[] = { "where_phrase", where_phrase ? where_phrase : "在家里", "child_name", mind_child_name(),
                                  "lang_name", lang_name, "fact", fact ? fact : "", NULL };
    mind_fill(system, system_cap, tpl, pairs);
    mem_release(tpl);
    return ESP_OK;
}

void mind_strip_tags(char *text)
{
    char *w = text;
    for (const char *r = text; *r;) {
        if (*r == '[') { const char *e = strchr(r, ']'); if (e && e - r < 24) { r = e + 1; continue; } }
        *w++ = *r++;
    }
    *w = '\0';
    while (w > text && (w[-1] == ' ' || w[-1] == '\n')) *--w = '\0';   // trailing blanks
    char *s = text; while (*s == ' ' || *s == '\n') s++;
    if (s != text) memmove(text, s, strlen(s) + 1);
}
