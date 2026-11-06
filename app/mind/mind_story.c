// mind_story.c — stories (2026-11-06, the owner: "小朋友喜欢听长故事，这里也需要看看怎么引入故事模式玩法"): whether the
// child's words ask for one, the prompt for each part (content/prompts/story.md, after today's facts in block C), and
// the shelf the stories told are kept on (data/stories/, the newest 30: the pairing page shows them, a story can be
// told again).
#define LOG_TAG "mind"
#include "mind/mind.h"
#include "mind_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/atomic_file.h"
#include "store/store.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SHELF_KEEP 30
char *mind_story_tpl;   // content/prompts/story.md, loaded with the other prompts

static bool any(const char *s, const char *const *words) { for (; *words; words++) if (strstr(s, *words)) return true; return false; }
static size_t utf8_count(const char *s) { size_t n = 0; for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++; return n; }
static bool word(const char *low, const char *w)   // an English word, not part of one ("no" is not in "know")
{
    size_t n = strlen(w);
    for (const char *p = strstr(low, w); p; p = strstr(p + 1, w))
        if ((p == low || !isalpha((unsigned char)p[-1])) && !isalpha((unsigned char)p[n])) return true;
    return false;
}

story_intent_t mind_story_intent(const char *said, const char *last_reply)
{
    if (!said || !*said) return STORY_INTENT_NONE;
    char low[256], last[512];
    size_t i = 0;
    for (; said[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)said[i]);
    low[i] = '\0';
    for (i = 0; last_reply && last_reply[i] && i < sizeof last - 1; i++) last[i] = (char)tolower((unsigned char)last_reply[i]);
    last[i] = '\0';
    static const char *const STOP[] = { "不听", "不想听", "不要听", "别讲", "不要讲", "不讲了", "停一下", "停下", "唔聽", "唔想聽", "唔好講", "stop", "no more", NULL };
    static const char *const MORE[] = { "继续", "接着讲", "然后呢", "后来呢", "还有呢", "繼續", "跟住", "然後呢", "後來呢", "仲有呢", "what happens next", "then what", "keep going", "go on", NULL };
    if (any(low, STOP)) return STORY_INTENT_STOP;
    static const char *const AGAIN[] = { "再讲一遍", "再讲一次", "再说一遍", "再讲一下刚才", "还要听刚才", "講多次", "再講一次", "再講一遍", "again", "one more time", "same story", NULL };
    if (any(low, AGAIN)) return STORY_INTENT_AGAIN;
    if (any(low, MORE)) return STORY_INTENT_MORE;
    static const char *const STORY[] = { "故事", "故仔", "story", "stories", NULL };
    static const char *const TELL[] = { "讲", "講", "说", "說", "听", "聽", "tell", "read", "hear", "another", "want", "time", "about", NULL };
    static const char *const TOLD[] = { "讲了", "讲过", "講咗", "講過", "听了", "听过", "聽咗", "聽過", "好听", "好聽", "told", NULL };   // telling about one, or liking it
    bool story = any(low, STORY);
    if (story && !any(low, TOLD) && (any(low, TELL) || utf8_count(said) <= 8)) return STORY_INTENT_START;
    if ((strstr(said, "再讲一个") || strstr(said, "再講一個") || strstr(low, "another one")) && any(last, STORY)) return STORY_INTENT_START;
    // Gugu just offered one ("要不要听个故事？", "你想听什么故事呀？"): a yes or a topic starts it, a no does not
    bool asked = strstr(last, "？") || strchr(last, '?') || strstr(last, "吗") || strstr(last, "嗎");
    if (any(last, STORY) && asked) {
        static const char *const NO_ZH[] = { "不", "唔", "别", "冇", NULL };
        if (!any(low, NO_ZH) && !word(low, "no") && !word(low, "not") && !word(low, "don't") && !word(low, "nope")) return STORY_INTENT_START;
    }
    return STORY_INTENT_NONE;
}

esp_err_t mind_story_prompt_build(mind_prompt_t *out, lang_t lang, int part, int parts, const char *child_said, bool bedtime, bool go_on)
{
    esp_err_t r = mind_prompt_build(out, lang, NULL, NULL);
    if (r != ESP_OK) return r;
    if (parts < 1) parts = 1;
    if (part < 1) part = 1;
    if (part > parts) part = parts;
    const char *name = mind_child_name();
    MEM_BIG_BSS static char said[400], open[600], close[600], rule[1300];   // off the engine's stack (one story at a time)
    if (child_said && *child_said) snprintf(said, sizeof said, "%s刚才回答：「%s」。把这个回答编进故事里，就算说得不太对，也要接住它。", name, child_said);
    else snprintf(said, sizeof said, "%s刚才没有回答，你就自己往下讲。", name);
    if (go_on) snprintf(open, sizeof open, "这一段刚才讲到一半被打断了：从你上面讲到的最后一句后面接着讲完这一段，不要从头再讲，也不要重复讲过的话。");
    else if (part == 1) snprintf(open, sizeof open, "用\"从前\"开头（或这门语言里同样的说法），讲清楚主角是谁、在哪里、遇到了什么事。");
    else snprintf(open, sizeof open, "接着上一段讲。%s", said);
    if (parts == 1) snprintf(close, sizeof close, "主角想办法解决了问题，给一个温暖的结尾。最后一句一定是\"故事讲完啦\"（用这一通电话的语言说），说完就停，不要再问问题。");
    else if (part == 1) snprintf(close, sizeof close, "这一段最后，问%s一个小问题，让%s猜接下来会怎样，或者帮主角出个主意，比如\"你猜，它推开门看见了谁？\"。问完就停，不要替%s回答。", name, name, name);
    else if (part < parts) snprintf(close, sizeof close, "这一段里主角想办法解决问题：第一次没成功，第二次差一点。这一段最后再问%s一个小问题，问完就停，不要替%s回答。", name, name);
    else snprintf(close, sizeof close, "这一段把问题解决，给一个温暖的结尾。最后一句一定是\"故事讲完啦\"（用这一通电话的语言说），说完就停，不要再问问题，也不要说\"明天\"还有什么。");
    snprintf(rule, sizeof rule, "%s%s", open, close);
    const char *length = go_on ? "讲完剩下的部分就好" : lang == LANG_EN ? "大约 220 个英文单词（一分半钟左右）" : "大约 350 个字（一分半钟左右）";
    char bed[240] = "";
    if (bedtime) snprintf(bed, sizeof bed, "- 现在是睡前：讲得慢一点、轻一点，%s安安静静、暖暖的，最后跟%s说晚安。", part >= parts ? "结尾" : "这一段", name);
    char part_s[12], parts_s[12];
    snprintf(part_s, sizeof part_s, "%d", part); snprintf(parts_s, sizeof parts_s, "%d", parts);
    const char *const pairs[] = { "child_name", name, "part", part_s, "parts", parts_s, "part_rule", rule, "length", length, "bedtime_rule", bed, NULL };
    MEM_BIG_BSS static char rules[3072], block[4608];
    mind_fill(rules, sizeof rules, mind_story_tpl ? mind_story_tpl : "【现在：讲故事】{part_rule}", pairs);
    snprintf(block, sizeof block, "%s\n\n%s", out->blocks[3].text, rules);
    out->blocks[3] = (chat_block_t){ block, false };
    return ESP_OK;
}

static void shelf_dir(char *dst, size_t cap) { snprintf(dst, cap, "%s/stories", store_data_dir()); }
static int by_name_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

char *mind_story_newest(lang_t lang)
{
    char names[30][40];
    int n = mind_story_shelf(names, 30);
    for (int i = 0; i < n; i++) {
        char path[200]; snprintf(path, sizeof path, "%s/stories/%.39s", store_data_dir(), names[i]);
        char *txt = file_read_all(path, NULL);
        cJSON *j = txt ? cJSON_Parse(txt) : NULL;
        if (txt) mem_release(txt);
        if (!j) continue;
        const cJSON *l = cJSON_GetObjectItem(j, "lang"), *t = cJSON_GetObjectItem(j, "tagged"), *plain = cJSON_GetObjectItem(j, "text");
        const char *body = cJSON_IsString(t) && *t->valuestring ? t->valuestring : cJSON_IsString(plain) ? plain->valuestring : NULL;
        char *out = NULL;
        if (body && cJSON_IsString(l) && !strcmp(l->valuestring, lang_code(lang))) {
            size_t len = strlen(body); out = mem_alloc_big(len + 1);
            if (out) memcpy(out, body, len + 1);
        }
        cJSON_Delete(j);
        if (out) return out;
    }
    return NULL;
}

int mind_story_shelf(char (*names)[40], int cap)
{
    char dir[160]; shelf_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0, room = 64;
    char (*all)[40] = malloc(sizeof *all * (size_t)room);
    for (struct dirent *e; all && (e = readdir(d));) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l >= 40 || strcmp(e->d_name + l - 5, ".json")) continue;
        if (n == room) { char (*more)[40] = realloc(all, sizeof *all * (size_t)(room *= 2)); if (!more) break; all = more; }
        snprintf(all[n++], 40, "%s", e->d_name);
    }
    closedir(d);
    if (!all) return 0;
    qsort(all, (size_t)n, sizeof *all, by_name_desc);
    int k = n < cap ? n : cap;
    if (names) for (int i = 0; i < k; i++) memcpy(names[i], all[i], 40);
    free(all);
    return k;
}

esp_err_t mind_story_save(lang_t lang, const char *text, char *path_out, size_t cap)
{
    char *clean = tag_strip(text ? text : "", NULL, 0, NULL);
    if (!clean) return ESP_ERR_NO_MEM;
    const char *start = clean; while (*start == ' ' || *start == '\n') start++;
    char title[96]; size_t t = 0;                                  // the first sentence names the story
    for (const char *p = start; *p && t < sizeof title - 4;) {
        size_t step = ((unsigned char)*p & 0xE0) == 0xC0 ? 2 : ((unsigned char)*p & 0xF0) == 0xE0 ? 3 : ((unsigned char)*p & 0xF8) == 0xF0 ? 4 : 1;
        if (t + step >= sizeof title - 1) break;
        memcpy(title + t, p, step); t += step;
        bool end = (step == 3 && (!memcmp(p, "。", 3) || !memcmp(p, "！", 3) || !memcmp(p, "？", 3))) || (step == 1 && (*p == '.' || *p == '!' || *p == '?'));
        p += step;
        if (end) break;
    }
    title[t] = '\0';
    char dir[160]; shelf_dir(dir, sizeof dir);
    mkdir(dir, 0755);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char stamp[32], iso[32]; strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm); strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S", &tm);
    char path[200];
    int next = 0;                                                  // two in one second keep their order: after the highest
    for (int k = 0; k < 100; k++) { snprintf(path, sizeof path, "%s/%s-%02d.json", dir, stamp, k); if (file_exists(path)) next = k + 1; }   // there, not in a gap the shelf left
    snprintf(path, sizeof path, "%s/%s-%02d.json", dir, stamp, next < 100 ? next : 99);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", iso); cJSON_AddStringToObject(o, "lang", lang_code(lang));
    cJSON_AddStringToObject(o, "title", title); cJSON_AddStringToObject(o, "text", start);
    cJSON_AddStringToObject(o, "tagged", text ? text : "");          // told again with its faces (mind_story_newest)
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o); mem_release(clean);
    bool ok = json && atomic_file_write(path, json, strlen(json));
    free(json);
    if (!ok) return ESP_FAIL;
    if (path_out && cap) snprintf(path_out, cap, "%s", path);
    char (*names)[40] = malloc(sizeof *names * 128);               // the newest SHELF_KEEP stay
    int n = names ? mind_story_shelf(names, 128) : 0;
    for (int i = SHELF_KEEP; i < n; i++) { char old[220]; snprintf(old, sizeof old, "%s/%s", dir, names[i]); remove(old); }
    free(names);
    LOGI("story kept on the shelf: %s (%s)", path, title);
    return ESP_OK;
}
