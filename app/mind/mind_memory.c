// mind_memory.c — the three memory books on flash: the journey log (one JSONL line per trip event: planned,
// each day, returned) and the two JSON lists the post-call extraction will fill (about the child, the grown
// canon). Reading them into prompt text, matching old journey entries to what the child just said, and
// forgetting on the parent's request. Files: memory/journey.jsonl, memory/about_child.json, memory/canon_grown.json.
#define LOG_TAG "mind"
#include "mind/mind.h"
#include "mind_internal.h"
#include "world/world.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/atomic_file.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static char s_dir[128];

static void journey_path(char *p, size_t cap) { snprintf(p, cap, "%s/memory/journey.jsonl", s_dir); }
static void book_path(char *p, size_t cap, const char *book) { snprintf(p, cap, "%s/memory/%s.json", s_dir, book); }

esp_err_t mind_memory_init(const char *data_dir)
{
    snprintf(s_dir, sizeof s_dir, "%s", data_dir ? data_dir : "");
    if (!s_dir[0]) return ESP_OK;
    const char *books[] = { "about_child", "canon_grown", NULL };
    for (int i = 0; books[i]; i++) {
        char p[200]; book_path(p, sizeof p, books[i]);
        if (!file_exists(p)) atomic_file_write(p, "[]", 2);
    }
    return ESP_OK;
}

#define PUT(...) do { int w = snprintf(dst + o, o < cap ? cap - o : 0, __VA_ARGS__); if (w > 0) o += (size_t)w; } while (0)

// Cut a UTF-8 string so it is at most `max` bytes and never ends inside a multi-byte character.
void mind_utf8_cut(char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max) return;
    size_t i = max;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    s[i] = '\0';
}

static const char *kind_name(journey_kind_t k) { return k == JOURNEY_PLANNED ? "planned" : k == JOURNEY_DAY ? "day" : "returned"; }
static const char *kind_zh(const char *k) { return !strcmp(k, "vacation") ? "度假" : !strcmp(k, "visit") ? "探亲" : "送信"; }
static const char *S(const cJSON *j, const char *k) { const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(j, k)); return v ? v : ""; }

static void md(const char *ymd, char *out, size_t cap)   // "2026-11-04" → "11月4日"
{
    if (strlen(ymd) == 10) snprintf(out, cap, "%d月%d日", atoi(ymd + 5), atoi(ymd + 8)); else snprintf(out, cap, "%s", ymd);
}

// ---- the journey log ----
esp_err_t mind_journey_add(journey_kind_t k, const world_trip_t *t, const char *date, int day, const char *text, const char (*hooks)[160], int n_hooks)
{
    if (!s_dir[0]) return ESP_ERR_INVALID_STATE;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", kind_name(k));
    cJSON_AddStringToObject(j, "trip", t->id);
    cJSON_AddStringToObject(j, "date", date);
    if (day > 0) cJSON_AddNumberToObject(j, "day", day);
    cJSON_AddStringToObject(j, "kind", world_kind_name(t->kind));
    cJSON_AddStringToObject(j, "place", t->place.name);
    cJSON_AddStringToObject(j, "country", t->place.country);
    cJSON_AddStringToObject(j, "friend", t->friend_name);
    cJSON_AddStringToObject(j, "animal", t->friend_animal);
    cJSON_AddStringToObject(j, "depart", t->depart_date);
    cJSON_AddStringToObject(j, "return", t->return_date);
    cJSON_AddStringToObject(j, "text", text ? text : "");
    cJSON *h = cJSON_AddArrayToObject(j, "hooks");
    for (int i = 0; i < n_hooks; i++) if (hooks[i][0]) cJSON_AddItemToArray(h, cJSON_CreateString(hooks[i]));
    char *line = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!line) return ESP_ERR_NO_MEM;
    char p[200]; journey_path(p, sizeof p);
    bool ok = file_append(p, line, strlen(line)) && file_append(p, "\n", 1);
    cJSON_free(line);
    LOGI("journey: %s %s %s", kind_name(k), t->id, date);
    return ok ? ESP_OK : ESP_FAIL;
}

typedef struct { cJSON **items; int n; char *txt; } journey_t;

static bool journey_load(journey_t *jl)
{
    memset(jl, 0, sizeof *jl);
    if (!s_dir[0]) return false;
    char p[200]; journey_path(p, sizeof p);
    size_t n; jl->txt = file_read_all(p, &n);
    if (!jl->txt) return false;
    int lines = 1; for (const char *c = jl->txt; *c; c++) if (*c == '\n') lines++;
    jl->items = mem_alloc_big((size_t)lines * sizeof(cJSON *));
    if (!jl->items) { mem_release(jl->txt); jl->txt = NULL; return false; }
    for (char *l = strtok(jl->txt, "\n"); l; l = strtok(NULL, "\n")) {
        if (!*l) continue;
        cJSON *j = cJSON_Parse(l);
        if (j) jl->items[jl->n++] = j;
    }
    return jl->n > 0;
}

static void journey_free(journey_t *jl)
{
    for (int i = 0; i < jl->n; i++) cJSON_Delete(jl->items[i]);
    mem_release(jl->items); mem_release(jl->txt);
}

static bool is(const cJSON *j, const char *kind) { return !strcmp(S(j, "t"), kind); }

size_t mind_journey_recent(char *dst, size_t cap, int trips)
{
    size_t o = 0; dst[0] = '\0';
    journey_t jl;
    bool any = journey_load(&jl);
    PUT("【旅程志】\n");
    int count = 0; const cJSON *last_returned = NULL;
    for (int i = jl.n - 1; i >= 0; i--) {
        const cJSON *j = jl.items[i];
        if (!is(j, "returned")) continue;
        if (!last_returned) last_returned = j;
        if (count >= trips) continue;
        char a[16], b[16]; md(S(j, "depart"), a, sizeof a); md(S(j, "return"), b, sizeof b);
        PUT("- %s到%s去%s%s：%s\n", a, b, S(j, "place"), kind_zh(S(j, "kind")), S(j, "text"));
        count++;
    }
    const world_trip_t *cur = world_trip_lock();
    int days = 0;
    if (cur) {
        for (int i = 0; i < jl.n; i++) {
            const cJSON *j = jl.items[i];
            if (!is(j, "day") || strcmp(S(j, "trip"), cur->id)) continue;
            const cJSON *d = cJSON_GetObjectItem(j, "day");
            PUT("- 这趟第 %d 天（%s）：%s\n", cJSON_IsNumber(d) ? (int)d->valuedouble : 0, S(j, "date"), S(j, "text"));
            days++;
        }
        world_trip_unlock();
    }
    if (last_returned) {
        const cJSON *h = cJSON_GetObjectItem(last_returned, "hooks");
        if (cJSON_IsArray(h) && cJSON_GetArraySize(h) > 0) {
            PUT("还没兑现的线头：");
            const cJSON *it; bool first = true;
            cJSON_ArrayForEach(it, h) { if (cJSON_IsString(it)) { PUT("%s%s", first ? "" : "；", it->valuestring); first = false; } }
            PUT("\n");
        }
    }
    if (!count && !days) PUT("（还没出过门，这是第一趟。）\n");
    if (any) journey_free(&jl);
    return (size_t)(count + days);
}

size_t mind_journey_days_text(const char *trip_id, char *dst, size_t cap)
{
    size_t o = 0; dst[0] = '\0';
    journey_t jl; size_t n = 0;
    if (!journey_load(&jl)) return 0;
    for (int i = 0; i < jl.n; i++) {
        const cJSON *j = jl.items[i];
        if (!is(j, "day") || strcmp(S(j, "trip"), trip_id)) continue;
        const cJSON *d = cJSON_GetObjectItem(j, "day");
        PUT("第 %d 天（%s）：%s\n", cJSON_IsNumber(d) ? (int)d->valuedouble : 0, S(j, "date"), S(j, "text"));
        n++;
    }
    journey_free(&jl);
    return n;
}

size_t mind_journey_match(const char *utterance, char *dst, size_t cap, int max)
{
    size_t o = 0; dst[0] = '\0';
    if (!utterance || !*utterance) return 0;
    journey_t jl;
    if (!journey_load(&jl)) return 0;
    const char *seen[8]; int ns = 0, found = 0;
    for (int i = jl.n - 1; i >= 0 && found < max; i--) {
        const cJSON *j = jl.items[i];
        if (is(j, "planned")) continue;
        const char *place = S(j, "place"), *friend = S(j, "friend"), *trip = S(j, "trip");
        bool hit = (strlen(place) >= 2 && strstr(utterance, place)) || (strlen(friend) >= 2 && strstr(utterance, friend));
        if (!hit) continue;
        bool dup = false;
        for (int k = 0; k < ns; k++) if (!strcmp(seen[k], trip)) dup = true;
        if (dup) continue;                       // one entry per trip, the newest (returned beats day)
        if (ns < 8) seen[ns++] = trip;
        if (!found) PUT("【你想起来的事】\n");
        char a[16]; md(S(j, "depart"), a, sizeof a);
        PUT("- %s去%s%s：%s\n", a, place, kind_zh(S(j, "kind")), S(j, "text"));
        found++;
    }
    journey_free(&jl);
    return (size_t)found;
}

bool mind_journey_has(journey_kind_t k, const char *trip_id, const char *date)
{
    journey_t jl; bool found = false;
    if (!journey_load(&jl)) return false;
    for (int i = 0; i < jl.n && !found; i++) {
        const cJSON *j = jl.items[i];
        found = is(j, kind_name(k)) && !strcmp(S(j, "trip"), trip_id) && (!date || !strcmp(S(j, "date"), date));
    }
    journey_free(&jl);
    return found;
}

esp_err_t mind_journey_forget(const char *trip_id)
{
    if (!s_dir[0] || !trip_id || !*trip_id) return ESP_ERR_INVALID_ARG;
    char p[200]; journey_path(p, sizeof p);
    size_t n; char *txt = file_read_all(p, &n);
    if (!txt) return ESP_OK;
    strbuf_t b; strbuf_init(&b);
    char needle[64]; snprintf(needle, sizeof needle, "\"trip\":\"%s\"", trip_id);
    int dropped = 0;
    for (char *l = strtok(txt, "\n"); l; l = strtok(NULL, "\n")) {
        if (!*l) continue;
        if (strstr(l, needle)) { dropped++; continue; }
        strbuf_puts(&b, l); strbuf_putc(&b, '\n');
    }
    mem_release(txt);
    bool ok = atomic_file_write(p, b.s ? b.s : "", b.len);
    strbuf_free(&b);
    LOGI("journey: forgot %s (%d entries)", trip_id, dropped);
    return ok ? ESP_OK : ESP_FAIL;
}

// ---- the two lists (filled by the post-call extraction later) ----
size_t mind_book_text(const char *book, char *dst, size_t cap)
{
    size_t o = 0; dst[0] = '\0';
    if (!s_dir[0]) return 0;
    char p[200]; book_path(p, sizeof p, book);
    size_t n; char *txt = file_read_all(p, &n);
    if (!txt) return 0;
    cJSON *arr = cJSON_Parse(txt); mem_release(txt);
    size_t count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const char *text = cJSON_IsString(it) ? it->valuestring : S(it, "text");
        if (text && *text) { PUT("- %s\n", text); count++; }
    }
    cJSON_Delete(arr);
    return count;
}

esp_err_t mind_book_forget(const char *book, int index)
{
    if (!s_dir[0]) return ESP_ERR_INVALID_STATE;
    char p[200]; book_path(p, sizeof p, book);
    size_t n; char *txt = file_read_all(p, &n);
    if (!txt) return ESP_ERR_NOT_FOUND;
    cJSON *arr = cJSON_Parse(txt); mem_release(txt);
    if (!cJSON_IsArray(arr) || index < 0 || index >= cJSON_GetArraySize(arr)) { cJSON_Delete(arr); return ESP_ERR_NOT_FOUND; }
    cJSON_DeleteItemFromArray(arr, index);
    char *out = cJSON_Print(arr); cJSON_Delete(arr);
    bool ok = out && atomic_file_write(p, out, strlen(out));
    cJSON_free(out);
    return ok ? ESP_OK : ESP_FAIL;
}

// ---- what was said on a day: the raw material for the diary and the recap ----
size_t mind_calls_digest(const char *date, char *dst, size_t cap)
{
    size_t o = 0; dst[0] = '\0';
    if (!s_dir[0]) return 0;
    char p[200]; snprintf(p, sizeof p, "%s/calls/index.jsonl", s_dir);
    size_t n; char *idx = file_read_all(p, &n);
    if (!idx) return 0;
    size_t turns = 0;
    for (char *l = strtok(idx, "\n"); l && turns < 4; l = strtok(NULL, "\n")) {
        cJSON *row = cJSON_Parse(l);
        const cJSON *ms = cJSON_GetObjectItem(row, "started_ms");
        const char *file = S(row, "file");
        if (cJSON_IsNumber(ms) && *file) {
            time_t t = (time_t)(ms->valuedouble / 1000); struct tm tm; localtime_r(&t, &tm);
            char d[11]; snprintf(d, sizeof d, "%04u-%02u-%02u", (unsigned)(tm.tm_year + 1900) % 10000u, (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u);
            if (!strcmp(d, date)) {
                char fp[300]; snprintf(fp, sizeof fp, "%s/%s", s_dir, file);
                size_t fn; char *ftxt = file_read_all(fp, &fn);
                for (char *tl = ftxt ? strtok(ftxt, "\n") : NULL; tl && turns < 4; tl = strtok(NULL, "\n")) {
                    cJSON *turn = cJSON_Parse(tl);
                    char said[100], reply[100];
                    snprintf(said, sizeof said, "%s", S(turn, "transcript")); mind_utf8_cut(said, 72);
                    snprintf(reply, sizeof reply, "%s", S(turn, "reply")); mind_utf8_cut(reply, 72);
                    if (said[0]) { PUT("孩子说\"%s\"，你说\"%s\"。", said, reply); turns++; }
                    cJSON_Delete(turn);
                }
                mem_release(ftxt);
            }
        }
        cJSON_Delete(row);
    }
    mem_release(idx);
    return turns;
}
