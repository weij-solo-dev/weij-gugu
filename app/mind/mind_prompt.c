// mind_prompt.c — loads prompt content once, assembles the blocks per turn with {placeholders} filled.
#define LOG_TAG "mind"
#include "mind/mind.h"
#include "mind_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/atomic_file.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static char *s_persona, *s_canon, *s_tags, *s_today_tpl;
static char *s_examples[4];          // indexed by lang_t
static char s_child_name[32] = "小朋友";
static int s_child_age_months;
MEM_BIG_BSS static char s_block_a[6144], s_block_a2[2048], s_block_b[512], s_block_c[768];

static char *load(const char *dir, const char *rel)
{
    char path[200];
    snprintf(path, sizeof path, "%s/prompts/%s", dir, rel);
    char *s = file_read_all(path, NULL);
    if (!s) LOGW("missing prompt file %s", path);
    return s;
}

esp_err_t mind_prompt_load(const char *content_dir)
{
    s_persona = load(content_dir, "persona.md");
    s_canon = load(content_dir, "canon_seed.md");
    s_tags = load(content_dir, "tags.md");
    s_today_tpl = load(content_dir, "today.md");
    s_examples[LANG_ZH] = load(content_dir, "examples/zh.md");
    s_examples[LANG_YUE] = load(content_dir, "examples/yue.md");
    s_examples[LANG_EN] = load(content_dir, "examples/en.md");
    return s_persona ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void mind_set_child(const char *name, int age_months)
{
    if (name && *name) snprintf(s_child_name, sizeof s_child_name, "%s", name);
    s_child_age_months = age_months;
}

// Replace {key} occurrences; unknown keys are left as-is. Pairs: key, value, key, value, …, NULL.
void mind_fill(char *dst, size_t cap, const char *tpl, const char *const *pairs)
{
    size_t o = 0;
    for (const char *p = tpl; *p && o + 1 < cap;) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (e && (size_t)(e - p) < 32) {
                size_t kl = (size_t)(e - p - 1);
                const char *val = NULL;
                for (int i = 0; pairs[i]; i += 2) if (strlen(pairs[i]) == kl && !strncmp(pairs[i], p + 1, kl)) { val = pairs[i + 1]; break; }
                if (val) { o += (size_t)snprintf(dst + o, cap - o, "%s", val); p = e + 1; continue; }
            }
        }
        dst[o++] = *p++;
    }
    dst[o < cap ? o : cap - 1] = '\0';
}

static const char *daypart(int hour)
{
    if (hour < 6) return "深夜";
    if (hour < 9) return "早上";
    if (hour < 12) return "上午";
    if (hour < 14) return "中午";
    if (hour < 18) return "下午";
    if (hour < 21) return "晚上";
    return "夜里";
}

const char *mind_mood_sentence(void)
{
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    if (tm.tm_hour < 7) return "你有点困，说话慢一点。";
    if (tm.tm_hour >= 20) return "快到睡觉时间了，你也有点困。";
    return "你今天心情不错。";
}

esp_err_t mind_prompt_build(mind_prompt_t *out, lang_t reply_lang, const char *where, const char *events)
{
    if (!s_persona) return ESP_ERR_INVALID_STATE;
    char age[64] = "";
    if (s_child_age_months > 0) snprintf(age, sizeof age, "，%u 岁%s", (unsigned)(s_child_age_months / 12) % 100u, (s_child_age_months % 12) >= 6 ? "半" : "");
    const char *const pairs_a[] = { "child_name", s_child_name, NULL };
    // A: persona + seed canon + tag vocabulary (stable for months)
    MEM_BIG_BSS static char persona[4096]; mind_fill(persona, sizeof persona, s_persona, pairs_a);
    snprintf(s_block_a, sizeof s_block_a, "%s\n\n【设定集】\n%s\n\n【表情标签】\n%s", persona, s_canon ? s_canon : "", s_tags ? s_tags : "");
    // A2: examples in the reply language (stable per language)
    const char *ex = (reply_lang > LANG_UNKNOWN && reply_lang <= LANG_YUE && s_examples[reply_lang]) ? s_examples[reply_lang] : s_examples[LANG_ZH];
    const char *lang_line = reply_lang == LANG_YUE ? "这一通电话孩子在说粤语，你用口语粤语回答。" :
                            reply_lang == LANG_EN ? "The child is speaking English on this call; answer in simple English." :
                            "这一通电话孩子在说普通话，你用普通话回答。";
    snprintf(s_block_a2, sizeof s_block_a2, "%s\n\n【对话示例】\n%s", lang_line, ex ? ex : "");
    // B: memory books (empty until milestone 3)
    snprintf(s_block_b, sizeof s_block_b, "【你记得的事】\n（还没有，这是你们最早的几次聊天。）");
    // C: today
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char date[40], weekday[16];
    static const char *const WD[] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };
    snprintf(date, sizeof date, "%u年%u月%u日", (unsigned)(tm.tm_year + 1900) % 10000u, (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u);
    snprintf(weekday, sizeof weekday, "%s", WD[tm.tm_wday]);
    char age_phrase[64]; memcpy(age_phrase, age, sizeof age); age_phrase[sizeof age - 1] = 0;
    const char *const pairs_c[] = { "date", date, "weekday", weekday, "daypart", daypart(tm.tm_hour), "child_name", s_child_name,
                                    "age_phrase", age_phrase, "where", where ? where : "在家里", "mood", mind_mood_sentence(),
                                    "events", events ? events : "", NULL };
    mind_fill(s_block_c, sizeof s_block_c, s_today_tpl ? s_today_tpl : "今天是{date}。你现在{where}。{mood}{events}", pairs_c);

    out->count = 4;
    out->blocks[0] = (chat_block_t){ s_block_a, true };
    out->blocks[1] = (chat_block_t){ s_block_a2, true };
    out->blocks[2] = (chat_block_t){ s_block_b, true };
    out->blocks[3] = (chat_block_t){ s_block_c, false };
    return ESP_OK;
}
