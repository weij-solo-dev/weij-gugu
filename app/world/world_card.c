// world_card.c — the trip card on the wire and on flash: JSON ⇄ world_trip_t, with the validation that
// decides whether what the model wrote is a trip Gugu can take.
#define LOG_TAG "world"
#include "world/world.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

const char *world_kind_name(world_kind_t k)
{
    static const char *const N[] = { "deliver", "vacation", "visit" };
    return k <= WORLD_KIND_VISIT ? N[k] : "deliver";
}

world_kind_t world_kind_parse(const char *s)
{
    if (s && !strcmp(s, "vacation")) return WORLD_KIND_VACATION;
    if (s && !strcmp(s, "visit")) return WORLD_KIND_VISIT;
    return WORLD_KIND_DELIVER;
}

const char *world_phase_name(world_phase_t p)
{
    static const char *const N[] = { "none", "planned", "away", "done" };
    return p <= WORLD_TRIP_DONE ? N[p] : "?";
}

const char *world_tod_name(world_tod_t t)
{
    static const char *const N[] = { "night", "sunrise", "day", "dusk" };
    return t <= WORLD_TOD_DUSK ? N[t] : "?";
}

static void copy_str(char *dst, size_t cap, const cJSON *obj, const char *key)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    snprintf(dst, cap, "%s", v ? v : "");
}

// Non-empty strings of a JSON array into rows of a char[n][stride] table; returns the count.
static int copy_list(const cJSON *arr, char *rows, size_t stride, int max_rows)
{
    int n = 0;
    const cJSON *it;
    if (!cJSON_IsArray(arr)) return 0;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || !it->valuestring[0]) continue;
        if (n >= max_rows) break;
        snprintf(rows + (size_t)n * stride, stride, "%s", it->valuestring);
        n++;
    }
    return n;
}

static bool reject(char *why, size_t cap, const char *msg)
{
    if (why && cap) snprintf(why, cap, "%s", msg);
    return false;
}

bool world_trip_from_json(const char *json, size_t n, world_trip_t *out, char *why, size_t why_cap)
{
    cJSON *j = cJSON_ParseWithLength(json, n);
    if (!j) return reject(why, why_cap, "not valid JSON");
    memset(out, 0, sizeof *out);
    bool ok = false;
    copy_str(out->id, sizeof out->id, j, "id");
    out->kind = world_kind_parse(cJSON_GetStringValue(cJSON_GetObjectItem(j, "kind")));
    const cJSON *place = cJSON_GetObjectItem(j, "place");
    if (!cJSON_IsObject(place)) { reject(why, why_cap, "place missing"); goto done; }
    copy_str(out->place.name, sizeof out->place.name, place, "name");
    copy_str(out->place.country, sizeof out->place.country, place, "country");
    copy_str(out->place.landmark, sizeof out->place.landmark, place, "landmark");
    if (!out->place.name[0]) { reject(why, why_cap, "place.name missing"); goto done; }
    const cJSON *lat = cJSON_GetObjectItem(place, "lat"), *lon = cJSON_GetObjectItem(place, "lon"), *tz = cJSON_GetObjectItem(place, "tz_offset_min");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) || fabs(lat->valuedouble) > 90 || fabs(lon->valuedouble) > 180) { reject(why, why_cap, "place.lat/lon missing or out of range"); goto done; }
    out->place.lat = lat->valuedouble; out->place.lon = lon->valuedouble;
    if (!cJSON_IsNumber(tz) || tz->valuedouble < -840 || tz->valuedouble > 840) { reject(why, why_cap, "place.tz_offset_min missing or out of range"); goto done; }
    out->place.tz_offset_min = (int)tz->valuedouble;
    copy_str(out->reason, sizeof out->reason, j, "reason");
    copy_str(out->recipient, sizeof out->recipient, j, "recipient");
    copy_str(out->letter, sizeof out->letter, j, "letter");
    copy_str(out->depart_date, sizeof out->depart_date, j, "depart_date");
    copy_str(out->return_date, sizeof out->return_date, j, "return_date");
    copy_str(out->souvenir, sizeof out->souvenir, j, "souvenir");
    copy_str(out->scene_prompt, sizeof out->scene_prompt, j, "scene_prompt");
    copy_str(out->ground, sizeof out->ground, j, "ground");
    copy_str(out->generated_at, sizeof out->generated_at, j, "generated_at");
    copy_str(out->prev_link, sizeof out->prev_link, j, "prev_link");
    copy_str(out->recap, sizeof out->recap, j, "recap");
    out->hook_count = copy_list(cJSON_GetObjectItem(j, "hooks"), &out->hooks[0][0], sizeof out->hooks[0], 2);
    if (!out->reason[0]) { reject(why, why_cap, "reason missing"); goto done; }
    const cJSON *nights = cJSON_GetObjectItem(j, "nights");
    if (!cJSON_IsNumber(nights) || nights->valuedouble < 1 || nights->valuedouble > WORLD_MAX_NIGHTS) { reject(why, why_cap, "nights must be 1..3"); goto done; }
    out->nights = (int)nights->valuedouble;
    out->days_count = copy_list(cJSON_GetObjectItem(j, "days"), &out->days[0][0], sizeof out->days[0], WORLD_MAX_DAYS);
    out->fact_count = copy_list(cJSON_GetObjectItem(j, "facts"), &out->facts[0][0], sizeof out->facts[0], WORLD_MAX_FACTS);
    if (out->fact_count < 3) { reject(why, why_cap, "fewer than 3 facts"); goto done; }
    const cJSON *fr = cJSON_GetObjectItem(j, "friend");
    if (cJSON_IsObject(fr)) { copy_str(out->friend_name, sizeof out->friend_name, fr, "name"); copy_str(out->friend_animal, sizeof out->friend_animal, fr, "animal"); }
    out->object_count = copy_list(cJSON_GetObjectItem(j, "objects"), &out->objects[0][0], sizeof out->objects[0], WORLD_MAX_OBJECTS);
    copy_list(cJSON_GetObjectItem(j, "postcards"), &out->postcards[0][0], sizeof out->postcards[0], WORLD_MAX_DAYS);
    ok = true;
done:
    cJSON_Delete(j);
    return ok;
}

static cJSON *str_array(const char *rows, size_t stride, int count)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(a, cJSON_CreateString(rows + (size_t)i * stride));
    return a;
}

char *world_trip_to_json(const world_trip_t *t)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", t->id);
    cJSON_AddStringToObject(j, "kind", world_kind_name(t->kind));
    cJSON *p = cJSON_AddObjectToObject(j, "place");
    cJSON_AddStringToObject(p, "name", t->place.name);
    cJSON_AddStringToObject(p, "country", t->place.country);
    cJSON_AddStringToObject(p, "landmark", t->place.landmark);
    cJSON_AddNumberToObject(p, "lat", t->place.lat);
    cJSON_AddNumberToObject(p, "lon", t->place.lon);
    cJSON_AddNumberToObject(p, "tz_offset_min", t->place.tz_offset_min);
    cJSON_AddStringToObject(j, "reason", t->reason);
    cJSON_AddStringToObject(j, "recipient", t->recipient);
    cJSON_AddStringToObject(j, "letter", t->letter);
    cJSON_AddStringToObject(j, "depart_date", t->depart_date);
    cJSON_AddStringToObject(j, "return_date", t->return_date);
    cJSON_AddNumberToObject(j, "nights", t->nights);
    cJSON_AddItemToObject(j, "days", str_array(&t->days[0][0], sizeof t->days[0], t->days_count));
    cJSON_AddItemToObject(j, "facts", str_array(&t->facts[0][0], sizeof t->facts[0], t->fact_count));
    cJSON *fr = cJSON_AddObjectToObject(j, "friend");
    cJSON_AddStringToObject(fr, "name", t->friend_name);
    cJSON_AddStringToObject(fr, "animal", t->friend_animal);
    cJSON_AddStringToObject(j, "scene_prompt", t->scene_prompt);
    cJSON_AddStringToObject(j, "ground", t->ground);
    cJSON_AddItemToObject(j, "objects", str_array(&t->objects[0][0], sizeof t->objects[0], t->object_count));
    cJSON_AddStringToObject(j, "souvenir", t->souvenir);
    int pc = 0; while (pc < WORLD_MAX_DAYS && t->postcards[pc][0]) pc++;
    cJSON_AddItemToObject(j, "postcards", str_array(&t->postcards[0][0], sizeof t->postcards[0], pc));
    cJSON_AddStringToObject(j, "prev_link", t->prev_link);
    cJSON_AddItemToObject(j, "hooks", str_array(&t->hooks[0][0], sizeof t->hooks[0], t->hook_count));
    cJSON_AddStringToObject(j, "recap", t->recap);
    cJSON_AddStringToObject(j, "generated_at", t->generated_at);
    char *txt = cJSON_Print(j);
    cJSON_Delete(j);
    if (!txt) return NULL;
    size_t n = strlen(txt);
    char *out = mem_alloc_big(n + 1);
    if (out) memcpy(out, txt, n + 1);
    cJSON_free(txt);
    return out;
}
