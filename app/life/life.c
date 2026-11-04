// life.c — a worker task runs every job (the vendor call needs a real stack); callers post and wait.
// Jobs: plan the next trip (a card from the model, linked to the journey so far), write the diary of each
// day away (the postcard plus what was said on the calls, no model call), write the recap when Gugu is
// back (a model call), and the daily tick that runs those three in order and is idempotent by date.
#define LOG_TAG "life"
#include "life/life.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/clock.h"
#include "foundation/task.h"
#include "foundation/strbuf.h"
#include "foundation/atomic_file.h"
#include "store/store.h"
#include "world/world.h"
#include "mind/mind.h"
#include "provider/provider.h"
#include "art/art.h"
#include "cJSON.h"
#include <sys/stat.h>
#include "foundation/wav.h"
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { JOB_PLAN = 1, JOB_TICK, JOB_DIARY, JOB_RECAP, JOB_ART, JOB_CHECK, JOB_LINE };
struct job {
    int kind;
    char kind_hint[16], place_hint[64], depart[11];
    int nights; uint32_t timeout_ms;
    char *why; size_t why_cap;
    esp_err_t result;
};

static mutex_t s_callers;          // one job at a time; the job lives in s_job while the worker runs it
static queue_t s_q;                // struct job *
static signal_t s_done;
static struct job s_job;
static bool s_worker;              // false until life_init: jobs then run on the caller (tests)
static int64_t s_last_attempt_us;
MEM_BIG_BSS static char s_journey[2048], s_days[2048];

#define WHY(...) do { if (j->why && j->why_cap) snprintf(j->why, j->why_cap, __VA_ARGS__); } while (0)

// ---- one model call that must come back as a JSON object (asked twice when it does not) ----
struct collect { strbuf_t text; bool failed; provider_error_t err; char stop[24]; uint32_t out_tokens; provider_usage_t usage; };
static void c_delta(const char *t, size_t n, void *u) { strbuf_append(&((struct collect *)u)->text, t, n); }
static void c_done(const char *stop, const provider_usage_t *us, void *u) { struct collect *c = u; snprintf(c->stop, sizeof c->stop, "%s", stop ? stop : ""); c->out_tokens = us ? us->out_tokens : 0; if (us) c->usage = *us; }
static void c_error(const provider_error_t *e, void *u) { struct collect *c = u; c->err = *e; c->failed = true; }

static bool extract_object(const char *s, const char **start, size_t *len)
{
    const char *a = strchr(s, '{'), *b = strrchr(s, '}');
    if (!a || !b || b < a) return false;
    *start = a; *len = (size_t)(b - a + 1);
    return true;
}

// On success *out holds the reply (caller strbuf_free()s) and js/jn point at the JSON object inside it.
static esp_err_t ask_json(struct job *j, const mind_trip_request_t *req, uint32_t max_tokens, strbuf_t *out, const char **js, size_t *jn, int64_t *ms)
{
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_CHAT);
    if (!v) { WHY("no chat vendor assigned"); return ESP_ERR_INVALID_STATE; }
    struct collect c = { 0 }; strbuf_init(&c.text);
    chat_events_t ev = { c_delta, c_done, c_error };
    chat_message_t m = { "user", req->user };
    chat_request_t cr = { .system = req->blocks, .system_count = req->count, .messages = &m, .message_count = 1,
                          .effort = CHAT_EFFORT_LOW, .max_tokens = max_tokens, .temperature = 0.9f };
    int64_t t0 = clock_us();
    *js = NULL; *jn = 0;
    esp_err_t err = ESP_OK;
    for (int attempt = 1; attempt <= 2 && !*js; attempt++) {   // an empty or JSON-less reply is asked again once
        strbuf_clear(&c.text); c.failed = false; c.stop[0] = '\0';
        chat_session_t *s = NULL;
        err = v->chat_open(&ev, &c, &s);
        if (err != ESP_OK) { WHY("chat open failed: %s", esp_err_to_name(err)); strbuf_free(&c.text); return err; }
        err = v->chat_send(s, &cr);      // returns when the stream has ended (the vendor's own deadlines bound it)
        v->chat_close(s);
        *ms = (clock_us() - t0) / 1000;
        if (err != ESP_OK && c.text.len == 0) { WHY("chat send failed: %s", esp_err_to_name(err)); strbuf_free(&c.text); return err; }
        if (c.failed && c.text.len == 0) { WHY("chat failed: %s (http %d, code %d)", c.err.message, c.err.http_status, c.err.api_code); strbuf_free(&c.text); return ESP_FAIL; }
        if (!c.text.s || !extract_object(c.text.s, js, jn)) {
            *js = NULL;
            LOGW("attempt %d: no JSON in %u bytes after %lld ms (stop %s, %u out tokens): %.120s", attempt, (unsigned)c.text.len, (long long)*ms, c.stop, (unsigned)c.out_tokens, c.text.s ? c.text.s : "");
            // The model sometimes thinks at length although thinking is switched off (seen on the device: 12 KB of
            // thinking, stop max_tokens, no text). Give the second attempt three times the room so the answer follows.
            if (!strcmp(c.stop, "max_tokens") && c.text.len == 0) cr.max_tokens = max_tokens * 3;
        }
    }
    if (!*js) { WHY("no JSON object in the reply after 2 attempts (%u bytes, %lld ms)", (unsigned)c.text.len, (long long)*ms); strbuf_free(&c.text); return ESP_ERR_INVALID_RESPONSE; }
    *out = c.text;
    return ESP_OK;
}

static void stamp(char *id, size_t id_cap, char *at, size_t at_cap)
{
    time_t now = world_now(); struct tm tm; localtime_r(&now, &tm);
    unsigned Y = (unsigned)(tm.tm_year + 1900) % 10000u, M = (unsigned)(tm.tm_mon + 1) % 100u, D = (unsigned)tm.tm_mday % 100u,
             h = (unsigned)tm.tm_hour % 100u, mi = (unsigned)tm.tm_min % 100u, sec = (unsigned)tm.tm_sec % 100u;
    if (id) snprintf(id, id_cap, "t%04u%02u%02u-%02u%02u%02u", Y, M, D, h, mi, sec);
    if (at) snprintf(at, at_cap, "%04u-%02u-%02uT%02u:%02u:%02u", Y, M, D, h, mi, sec);
}

// ---- plan the next trip ----
static esp_err_t do_plan(struct job *j)
{
    s_last_attempt_us = clock_us();
    char today[11], depart[11];
    world_today(today, sizeof today);
    if (j->depart[0]) snprintf(depart, sizeof depart, "%s", j->depart); else world_date_add(today, 1, depart, sizeof depart);
    char recent[256]; world_recent_places(recent, sizeof recent);
    mind_journey_recent(s_journey, sizeof s_journey, 3);
    mind_trip_request_t req;
    if (mind_trip_request_build(&req, j->kind_hint[0] ? j->kind_hint : NULL, j->place_hint[0] ? j->place_hint : NULL, depart, j->nights, recent, s_journey) != ESP_OK) {
        WHY("trip prompt missing (content/prompts/trip_card.md)"); return ESP_ERR_NOT_FOUND;
    }
    strbuf_t text; const char *js; size_t jn; int64_t ms = 0;
    esp_err_t err = ask_json(j, &req, 3000, &text, &js, &jn, &ms);
    if (err != ESP_OK) return err;
    MEM_BIG_BSS static world_trip_t t;    // one plan at a time (s_callers)
    char vwhy[96];
    if (!world_trip_from_json(js, jn, &t, vwhy, sizeof vwhy)) {
        WHY("trip card rejected: %s (%lld ms)", vwhy, (long long)ms);
        LOGW("rejected card: %.*s", (int)(jn > 400 ? 400 : jn), js);
        strbuf_free(&text); return ESP_ERR_INVALID_RESPONSE;
    }
    strbuf_free(&text);
    if (j->nights >= 1 && j->nights <= WORLD_MAX_NIGHTS) t.nights = j->nights;
    snprintf(t.depart_date, sizeof t.depart_date, "%s", depart);
    world_date_add(depart, t.nights, t.return_date, sizeof t.return_date);
    if (t.days_count < t.nights + 1) t.days_count = t.nights + 1;   // the missing days stay empty rather than shifting the plan
    stamp(t.id, sizeof t.id, t.generated_at, sizeof t.generated_at);
    err = world_trip_set(&t);
    if (err != ESP_OK) { WHY("could not save the trip card: %s", esp_err_to_name(err)); return err; }
    char planned[480];
    snprintf(planned, sizeof planned, "%s%s%s", t.reason, t.prev_link[0] ? " " : "", t.prev_link);
    mind_journey_add(JOURNEY_PLANNED, &t, today, 0, planned, t.hooks, t.hook_count);
    WHY("%s to %s (%s), %s..%s, %d nights, %d facts, %lld ms%s%s", world_kind_name(t.kind), t.place.name, t.place.country, t.depart_date, t.return_date,
        t.nights, t.fact_count, (long long)ms, t.prev_link[0] ? " | " : "", t.prev_link);
    LOGI("trip planned: %s", j->why ? j->why : t.place.name);
    return ESP_OK;
}

// ---- the diary: one entry per day away, from the postcard and the day's calls; no model call ----
static esp_err_t do_diary(struct job *j)
{
    world_state_t st; world_state(&st);
    const world_trip_t *t = world_trip_lock();
    if (!t) { WHY("no trip card"); return ESP_OK; }
    if (st.phase != WORLD_TRIP_AWAY && st.phase != WORLD_TRIP_DONE) { WHY("not away yet"); world_trip_unlock(); return ESP_OK; }
    char today[11]; world_today(today, sizeof today);
    int written = 0;
    int last = st.phase == WORLD_TRIP_DONE ? t->nights : st.day_index;
    if (last > t->nights) last = t->nights;
    for (int d = 0; d <= last; d++) {
        char date[11]; world_date_add(t->depart_date, d, date, sizeof date);
        if (strcmp(date, today) > 0) break;
        if (mind_journey_has(JOURNEY_DAY, t->id, date)) continue;
        MEM_BIG_BSS static char text[900], calls[400];
        const char *card_text = t->postcards[d][0] ? t->postcards[d] : t->days[d];
        mind_calls_digest(date, calls, sizeof calls);
        snprintf(text, sizeof text, "%s%s%s", card_text, calls[0] ? " 这天通话里：" : "", calls);
        if (mind_journey_add(JOURNEY_DAY, t, date, d + 1, text, NULL, 0) == ESP_OK) written++;
    }
    world_trip_unlock();
    WHY("%d day entr%s written", written, written == 1 ? "y" : "ies");
    return ESP_OK;
}

// ---- the recap: when Gugu is back, the model writes the trip into the journey ----
static esp_err_t do_recap(struct job *j)
{
    world_state_t st; world_state(&st);
    const world_trip_t *t = world_trip_lock();
    if (!t) { WHY("no trip card"); return ESP_OK; }
    if (st.phase != WORLD_TRIP_DONE) { WHY("not back yet (%s)", world_phase_name(st.phase)); world_trip_unlock(); return ESP_OK; }
    if (mind_journey_has(JOURNEY_RETURNED, t->id, NULL)) { WHY("recap already written"); world_trip_unlock(); return ESP_OK; }
    char id[40]; snprintf(id, sizeof id, "%s", t->id);
    world_trip_unlock();
    s_last_attempt_us = clock_us();
    mind_journey_days_text(id, s_days, sizeof s_days);
    mind_trip_request_t req;
    if (mind_recap_request_build(&req, s_days) != ESP_OK) { WHY("recap prompt missing (content/prompts/trip_recap.md)"); return ESP_ERR_NOT_FOUND; }
    strbuf_t text; const char *js; size_t jn; int64_t ms = 0;
    esp_err_t err = ask_json(j, &req, 1200, &text, &js, &jn, &ms);
    if (err != ESP_OK) return err;
    cJSON *r = cJSON_ParseWithLength(js, jn);
    const char *recap = cJSON_GetStringValue(cJSON_GetObjectItem(r, "recap"));
    if (!recap || !*recap) { WHY("recap JSON has no recap text (%lld ms)", (long long)ms); cJSON_Delete(r); strbuf_free(&text); return ESP_ERR_INVALID_RESPONSE; }
    char hooks[2][160] = { { 0 }, { 0 } }; int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(r, "open_hooks")) if (cJSON_IsString(it) && it->valuestring[0] && n < 2) snprintf(hooks[n++], sizeof hooks[0], "%s", it->valuestring);
    char today[11]; world_today(today, sizeof today);
    err = world_trip_set_recap(recap, hooks, n);
    t = world_trip_lock();
    if (t) { mind_journey_add(JOURNEY_RETURNED, t, today, 0, recap, hooks, n); world_trip_unlock(); }
    WHY("recap %u chars, %d open hook%s, %lld ms", (unsigned)strlen(recap), n, n == 1 ? "" : "s", (long long)ms);
    LOGI("recap: %s", recap);
    cJSON_Delete(r); strbuf_free(&text);
    return err;
}

// ---- the pictures: one image call per piece, each piece idempotent by its file (or its .fail marker) ----
static esp_err_t gen_image(const provider_vtable_t *v, const char *prompt, const char *out_path, const char *what, const char *trip_id)
{
    image_params_t p = { .prompt = prompt, .aspect = "1:1", .out_path = out_path };
    provider_usage_t u = { 0 }; provider_error_t e = { 0 };
    int64_t t0 = clock_us();
    esp_err_t r = v->image_generate(&p, &u, &e);
    if (r != ESP_OK) LOGW("image %s failed: %s (http %d, code %d)", what, e.message, e.http_status, e.api_code);
    else {
        char ledger[200]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
        snprintf(ledger, sizeof ledger, "{\"t\":\"%s\",\"art\":\"%s\",\"trip\":\"%s\",\"fen\":%.2f,\"ms\":%lld}", ts, what, trip_id, (double)provider_image_cost_fen(), (long long)((clock_us() - t0) / 1000));
        store_ledger_add(ledger);
    }
    return r;
}

static void mark_failed(const char *dir, const char *name) { char p[240]; snprintf(p, sizeof p, "%s/%s.fail", dir, name); atomic_file_write(p, "1", 1); }
static bool have_or_failed(const char *dir, const char *name)
{
    char p[240]; snprintf(p, sizeof p, "%s/%s.art", dir, name); if (file_exists(p)) return true;
    snprintf(p, sizeof p, "%s/%s.fail", dir, name); return file_exists(p);
}

static esp_err_t do_art(struct job *j)
{
    const world_trip_t *t = world_trip_lock();
    if (!t) { WHY("no trip card"); return ESP_OK; }
    char id[40], scene[400], ground[16], objs[3][24], animal[32]; int nobj = 0;
    snprintf(id, sizeof id, "%s", t->id); snprintf(scene, sizeof scene, "%s", t->scene_prompt); snprintf(ground, sizeof ground, "%s", t->ground); snprintf(animal, sizeof animal, "%s", t->friend_animal);
    for (int i = 0; i < t->object_count && nobj < 3; i++) if (t->objects[i][0]) snprintf(objs[nobj++], sizeof objs[0], "%s", t->objects[i]);
    world_trip_unlock();
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_IMAGE);
    if (!v || !v->image_generate) { WHY("no image vendor: the built-in scene stands in"); return ESP_OK; }
    char dir[160]; snprintf(dir, sizeof dir, "%s/art/%s", store_data_dir(), id);
    mkdir(dir, 0755);
    MEM_BIG_BSS static char style[400], prompt[900];
    int made = 0, failed = 0, there = 0; int64_t t0 = clock_us();
    char jpg[220], out[220];
    // the backdrop
    if (have_or_failed(dir, "backdrop")) there++;
    else if (scene[0]) {
        art_catalogue_style(store_content_dir(), "backdrop", style, sizeof style);
        snprintf(prompt, sizeof prompt, "%s, %s", scene, style);
        snprintf(jpg, sizeof jpg, "%s/backdrop.jpg", dir); snprintf(out, sizeof out, "%s/backdrop.art", dir);
        art_image_t img;
        if (gen_image(v, prompt, jpg, "backdrop", id) == ESP_OK && art_import_backdrop(jpg, art_ground_parse(ground), &img) == ESP_OK) {
            if (art_save(&img, out) == ESP_OK) made++; else failed++;
            art_free(&img); remove(jpg);
        } else { failed++; mark_failed(dir, "backdrop"); }
    }
    // the props and the friend
    for (int i = 0; i <= nobj; i++) {
        const char *name = i < nobj ? objs[i] : (animal[0] ? "friend" : NULL);
        if (!name) continue;
        art_catalogue_t e;
        if (i < nobj) { art_catalogue_lookup(store_content_dir(), objs[i], &e); if (e.builtin) continue; }
        else { snprintf(e.subject, sizeof e.subject, "a friendly %s", animal); e.size = 22; snprintf(e.bg, sizeof e.bg, "green"); }
        if (have_or_failed(dir, name)) { there++; continue; }
        art_catalogue_style(store_content_dir(), "prop", style, sizeof style);
        char *bg = strstr(style, "{bg}");
        if (bg) { char tail[300]; snprintf(tail, sizeof tail, "%s", bg + 4); snprintf(bg, sizeof style - (size_t)(bg - style), "%s%s", e.bg, tail); }
        snprintf(prompt, sizeof prompt, "%s, %s", e.subject, style);
        snprintf(jpg, sizeof jpg, "%s/%s.jpg", dir, name); snprintf(out, sizeof out, "%s/%s.art", dir, name);
        art_image_t img; art_prop_stats_t st;
        if (gen_image(v, prompt, jpg, name, id) == ESP_OK && art_import_prop(jpg, e.size, &img, &st) == ESP_OK) {
            if (st.passed && art_save(&img, out) == ESP_OK) made++; else { failed++; mark_failed(dir, name); }
            art_free(&img); remove(jpg);
        } else { failed++; mark_failed(dir, name); }
    }
    WHY("%d made, %d failed, %d already there, %lld s", made, failed, there, (long long)((clock_us() - t0) / 1000000));
    LOGI("art for %s: %s", id, j->why ? j->why : "");
    return failed && !made ? ESP_FAIL : ESP_OK;
}

// ---- the daily tick: diary, recap, then plan when due; planning is keyed by date ----
static esp_err_t do_tick(struct job *j)
{
    char why[160]; struct job sub = { .why = why, .why_cap = sizeof why };
    do_diary(&sub); LOGI("tick diary: %s", why);
    esp_err_t rerr = do_recap(&sub); LOGI("tick recap: %s (%s)", why, esp_err_to_name(rerr));
    world_state_t pre; world_state(&pre);
    if ((pre.phase == WORLD_TRIP_PLANNED || pre.phase == WORLD_TRIP_AWAY) && store_setting_int("auto_art", 1)) { do_art(&sub); LOGI("tick art: %s", why); }   // pictures a past night could not finish
    char today[11]; world_today(today, sizeof today);
    if (!strcmp(store_setting_str("life_tick_date", ""), today)) { WHY("already planned for %s", today); return ESP_OK; }
    world_state_t st; world_state(&st);
    bool due = st.phase == WORLD_TRIP_NONE;
    if (st.phase == WORLD_TRIP_DONE) {
        const world_trip_t *t = world_trip_lock();
        if (t) { due = world_date_diff(t->return_date, today) >= store_setting_int("home_days", 1); world_trip_unlock(); }
    }
    if (!due) { store_setting_set_str("life_tick_date", today); WHY("nothing due (%s)", world_phase_name(st.phase)); return ESP_OK; }
    if (s_last_attempt_us && clock_us() - s_last_attempt_us < 30 * 60 * 1000000LL && rerr != ESP_OK) { WHY("last attempt failed less than 30 min ago"); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = do_plan(j);
    LOGI("tick plan %s: %s (%s)", today, esp_err_to_name(err), j->why ? j->why : "");
    if (err == ESP_OK) store_setting_set_str("life_tick_date", today);
    if (err == ESP_OK && store_setting_int("auto_art", 1)) { do_art(&sub); LOGI("tick art: %s", why); }
    return err;
}

static signal_t s_tts_done;
struct tts_collect { int16_t *pcm; size_t n, cap; bool failed; provider_error_t err; uint32_t chars; };
static void tc_audio(const int16_t *pcm, size_t samples, void *u)
{
    struct tts_collect *t = u;
    size_t k = t->cap - t->n < samples ? t->cap - t->n : samples;
    memcpy(t->pcm + t->n, pcm, k * sizeof(int16_t)); t->n += k;
}
static void tc_flushed(const provider_usage_t *us, void *u) { struct tts_collect *t = u; t->chars += us ? us->chars : 0; signal_set(s_tts_done); }
static void tc_error(const provider_error_t *e, void *u) { struct tts_collect *t = u; t->err = *e; t->failed = true; signal_set(s_tts_done); }

// ---- the service check: is each role answering? (the bubble's "service" icon) ----
static atomic_uint s_check_roles;
static life_check_cb s_check_cb; static void *s_check_user;
static struct job s_check_job = { .kind = JOB_CHECK };

static bool check_chat(char *why, size_t cap, int *kind)
{
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_CHAT);
    if (!v) { snprintf(why, cap, "chat: no vendor assigned"); *kind = PERR_OTHER; return false; }
    struct collect c = { 0 }; strbuf_init(&c.text);
    chat_events_t ev = { c_delta, c_done, c_error };
    chat_block_t sys = { "Reply with the single word: ok", false };
    chat_message_t m = { "user", "ping" };
    chat_request_t cr = { .system = &sys, .system_count = 1, .messages = &m, .message_count = 1,
                          .effort = CHAT_EFFORT_LOW, .max_tokens = 8, .temperature = 0.0f };
    int64_t t0 = clock_us();
    chat_session_t *s = NULL;
    esp_err_t err = v->chat_open(&ev, &c, &s);
    if (err == ESP_OK) { err = v->chat_send(s, &cr); v->chat_close(s); }
    long long ms = (long long)((clock_us() - t0) / 1000);
    bool ok = false;
    if (c.failed) { *kind = c.err.kind; snprintf(why, cap, "chat: %.60s (http %d, code %d) after %lld ms", c.err.message, c.err.http_status, c.err.api_code, ms); }
    else if (err != ESP_OK) { *kind = PERR_NETWORK; snprintf(why, cap, "chat: %s after %lld ms", esp_err_to_name(err), ms); }
    else { ok = true; snprintf(why, cap, "chat answered in %lld ms", ms); }
    strbuf_free(&c.text);
    return ok;
}

static esp_err_t tts_speak(const provider_vtable_t *v, const char *text, lang_t lang, const char *voice, struct tts_collect *t, uint32_t timeout_ms);

static bool check_tts(char *why, size_t cap, int *kind)
{
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_TTS);
    if (!v) { snprintf(why, cap, "tts: no vendor assigned"); *kind = PERR_OTHER; return false; }
    lang_t lang = mind_primary_lang();
    struct tts_collect t = { 0 };
    t.cap = 16000 * 4;
    t.pcm = mem_alloc_big(t.cap * sizeof(int16_t));
    if (!t.pcm) { snprintf(why, cap, "tts: no memory"); *kind = PERR_OTHER; return false; }
    int64_t t0 = clock_us();
    esp_err_t err = tts_speak(v, lang == LANG_EN ? "OK." : "好。", lang, NULL, &t, 15000);
    long long ms = (long long)((clock_us() - t0) / 1000);
    bool ok = false;
    if (t.failed) { *kind = t.err.kind; snprintf(why, cap, "tts: %.60s (http %d, code %d) after %lld ms", t.err.message, t.err.http_status, t.err.api_code, ms); }
    else if (err != ESP_OK) { *kind = PERR_NETWORK; snprintf(why, cap, "tts: %s after %lld ms", esp_err_to_name(err), ms); }
    else if (t.n == 0) { *kind = PERR_OTHER; snprintf(why, cap, "tts: no audio after %lld ms", ms); }
    else { ok = true; snprintf(why, cap, "voice answered in %lld ms", ms); }
    mem_release(t.pcm);
    return ok;
}

struct asr_collect { bool failed; provider_error_t err; };
static void ac_final(const char *text, lang_t lang, const provider_usage_t *u, void *user) { (void)text; (void)lang; (void)u; (void)user; }
static void ac_error(const provider_error_t *e, void *user) { struct asr_collect *a = user; a->err = *e; a->failed = true; }

static bool check_asr(char *why, size_t cap, int *kind)
{
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_ASR);
    if (!v) { snprintf(why, cap, "asr: no vendor assigned"); *kind = PERR_OTHER; return false; }
    struct asr_collect a = { 0 };
    asr_params_t ap = { .lang_hint = mind_primary_lang(), .sample_rate = 16000 };
    asr_events_t ae = { NULL, ac_final, ac_error };
    int64_t t0 = clock_us();
    asr_session_t *s = NULL;
    esp_err_t err = v->asr_open(&ap, &ae, &a, &s);
    if (err == ESP_OK) {
        static const int16_t quiet[320];                      // 20 ms of silence, fed 25 times
        for (int i = 0; i < 25 && err == ESP_OK; i++) err = v->asr_feed(s, quiet, 320);
        if (err == ESP_OK) err = v->asr_finish(s);
        v->asr_close(s);
    }
    long long ms = (long long)((clock_us() - t0) / 1000);
    bool ok = false;
    if (a.failed) { *kind = a.err.kind; snprintf(why, cap, "asr: %.60s (http %d, code %d) after %lld ms", a.err.message, a.err.http_status, a.err.api_code, ms); }
    else if (err != ESP_OK) { *kind = PERR_NETWORK; snprintf(why, cap, "asr: %s after %lld ms", esp_err_to_name(err), ms); }
    else { ok = true; snprintf(why, cap, "recognizer answered in %lld ms", ms); }
    return ok;
}

static void do_check(void)
{
    unsigned roles = atomic_exchange(&s_check_roles, 0);
    if (!roles) return;
    life_check_cb cb = s_check_cb; void *user = s_check_user;
    char why[120] = "", part[120]; int kind = PERR_NONE; bool ok = true;
    if (ok && (roles & LIFE_CHECK_CHAT)) { ok = check_chat(part, sizeof part, &kind); snprintf(why, sizeof why, "%s", part); }
    if (ok && (roles & LIFE_CHECK_TTS)) { ok = check_tts(part, sizeof part, &kind); snprintf(why, sizeof why, "%s", part); }
    if (ok && (roles & LIFE_CHECK_ASR)) { ok = check_asr(part, sizeof part, &kind); snprintf(why, sizeof why, "%s", part); }
    LOGI("service check (%s%s%s): %s (%s)", roles & LIFE_CHECK_CHAT ? "chat" : "", roles & LIFE_CHECK_TTS ? (roles & LIFE_CHECK_CHAT ? "+voice" : "voice") : "",
         roles & LIFE_CHECK_ASR ? (roles & (LIFE_CHECK_CHAT | LIFE_CHECK_TTS) ? "+recognizer" : "recognizer") : "", ok ? "ok" : "failed", why);
    if (cb) cb(ok, kind, why, user);
}

// ---- the bubble's lines ----
static struct { atomic_bool pending; lang_t lang; life_line_cb cb; void *user; } s_line[3];   // 0: hello, 1: idea, 2: a voice preview
static char s_sample_voice[64];                                // the voice the preview tries
static struct job s_line_job = { .kind = JOB_LINE };

// One utterance through the TTS role into `t` (open, text, flush, wait for the flush or an error, close).
static esp_err_t tts_speak(const provider_vtable_t *v, const char *text, lang_t lang, const char *voice, struct tts_collect *t, uint32_t timeout_ms)
{
    if (!s_tts_done) s_tts_done = signal_create();
    while (signal_wait(s_tts_done, 0)) { }                    // drop a stale latch
    tts_events_t ev = { NULL, tc_audio, NULL, tc_flushed, tc_error };
    tts_params_t p = { .voice_id = voice, .lang = lang, .speed = 1.0f, .sample_rate = 16000 };   // NULL: the family's pick or the preset's
    tts_session_t *s = NULL;
    esp_err_t err = v->tts_open(&p, &ev, t, &s);
    if (err == ESP_OK) {
        err = v->tts_text(s, text, strlen(text));
        if (err == ESP_OK) err = v->tts_flush(s);
        if (err == ESP_OK && !signal_wait(s_tts_done, timeout_ms)) err = ESP_ERR_TIMEOUT;
        v->tts_close(s);
    }
    if (err == ESP_OK && t->failed) err = ESP_FAIL;
    return err;
}

// Speak `text` in `lang` with the TTS role and keep it as a 16 kHz mono WAV at `path` (atomically). One ledger line.
static esp_err_t synth_to_wav(const char *what, const char *text, lang_t lang, const char *voice, const char *path, const provider_usage_t *chat, char *why, size_t why_cap)
{
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_TTS);
    if (!v) { snprintf(why, why_cap, "no tts vendor assigned"); return ESP_ERR_INVALID_STATE; }
    struct tts_collect t = { 0 };
    t.cap = 16000 * 40;                                       // 40 s at most
    uint8_t *buf = mem_alloc_big(WAV_HEADER_BYTES + t.cap * sizeof(int16_t));
    if (!buf) { snprintf(why, why_cap, "no memory for the clip"); return ESP_ERR_NO_MEM; }
    t.pcm = (int16_t *)(buf + WAV_HEADER_BYTES);
    int64_t t0 = clock_us();
    esp_err_t err = tts_speak(v, text, lang, voice, &t, 30000);
    if (err == ESP_OK && t.n == 0) { err = ESP_ERR_INVALID_RESPONSE; snprintf(why, why_cap, "the voice came back empty"); }
    else if (err != ESP_OK) snprintf(why, why_cap, "tts: %.60s (%s)", t.failed ? t.err.message : "", esp_err_to_name(err));
    if (err == ESP_OK) {
        wav_write_header(buf, 16000, 1, (uint32_t)(t.n * sizeof(int16_t)));
        if (!atomic_file_write(path, buf, WAV_HEADER_BYTES + t.n * sizeof(int16_t))) { err = ESP_FAIL; snprintf(why, why_cap, "could not write the clip (%.60s)", path); }
        else {
            provider_usage_t ut = { .chars = t.chars ? t.chars : (uint32_t)strlen(text) / 3 }, ua = { 0 }, uc = chat ? *chat : (provider_usage_t){ 0 };
            char ledger[200], ts[24]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
            snprintf(ledger, sizeof ledger, "{\"t\":\"%s\",\"voice\":\"%s\",\"fen\":%.3f,\"tts_chars\":%u,\"in\":%u,\"out\":%u}", ts, what,
                     (double)provider_turn_cost_fen(&ua, &uc, &ut), (unsigned)ut.chars, (unsigned)uc.in_tokens, (unsigned)uc.out_tokens);
            store_ledger_add(ledger);
            snprintf(why, why_cap, "%u ms of voice in %lld ms", (unsigned)(t.n * 1000 / 16000), (long long)((clock_us() - t0) / 1000));
        }
    }
    mem_release(buf);
    return err;
}

// One sentence from the chat role for a system prompt (no JSON): the share's retelling.
static esp_err_t ask_line(const char *system, char *out, size_t cap, provider_usage_t *usage, char *why, size_t why_cap)
{
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_CHAT);
    if (!v) { snprintf(why, why_cap, "no chat vendor assigned"); return ESP_ERR_INVALID_STATE; }
    struct collect c = { 0 }; strbuf_init(&c.text);
    chat_events_t ev = { c_delta, c_done, c_error };
    chat_block_t sys = { system, false };
    chat_message_t m = { "user", "说吧。" };
    chat_request_t cr = { .system = &sys, .system_count = 1, .messages = &m, .message_count = 1, .effort = CHAT_EFFORT_LOW, .max_tokens = 300, .temperature = 0.9f };
    chat_session_t *s = NULL;
    esp_err_t err = v->chat_open(&ev, &c, &s);
    if (err == ESP_OK) { err = v->chat_send(s, &cr); v->chat_close(s); }
    if (c.failed) { snprintf(why, why_cap, "chat: %.60s (http %d)", c.err.message, c.err.http_status); err = ESP_FAIL; }
    else if (err == ESP_OK && (!c.text.s || !c.text.len)) { snprintf(why, why_cap, "chat: empty reply (stop %s)", c.stop); err = ESP_ERR_INVALID_RESPONSE; }
    if (err == ESP_OK) { snprintf(out, cap, "%s", c.text.s); mind_strip_tags(out); *usage = c.usage; }
    strbuf_free(&c.text);
    return err;
}

static void do_line(int which)
{
    if (!atomic_exchange(&s_line[which].pending, false)) return;
    life_line_cb cb = s_line[which].cb; void *user = s_line[which].user; lang_t lang = s_line[which].lang;
    life_line_t L = { 0 };
    provider_usage_t chat = { 0 };
    esp_err_t err = ESP_OK;
    if (which == 2) {
        snprintf(L.path, sizeof L.path, "%s/voice/sample.wav", store_data_dir());
        err = mind_line_sample(lang, L.text, sizeof L.text);
        if (err != ESP_OK) snprintf(L.why, sizeof L.why, "no preview line for %s", lang_code(lang));
    } else if (which == 0) {
        snprintf(L.path, sizeof L.path, "%s/voice/hello.wav", store_data_dir());
        err = mind_line_hello(lang, L.text, sizeof L.text);
        if (err != ESP_OK) snprintf(L.why, sizeof L.why, "no self-introduction for %s", lang_code(lang));
    } else {
        snprintf(L.path, sizeof L.path, "%s/voice/idea.wav", store_data_dir());
        int n = store_setting_int("idea_n", 0);
        char fact[200] = "", where[120] = "";
        const world_trip_t *t = world_trip_lock();
        if (t && t->fact_count > 0 && n % 2 == 0) {          // a share on even turns, while there is a trip to talk about
            snprintf(fact, sizeof fact, "%s", t->facts[(n / 2) % t->fact_count]);
            world_state_t st; world_state(&st);
            snprintf(where, sizeof where, st.phase == WORLD_TRIP_AWAY ? "正在%s" : st.phase == WORLD_TRIP_PLANNED ? "过几天要去%s" : "刚从%s回来", t->place.name);
        }
        world_trip_unlock();
        if (fact[0]) {
            MEM_BIG_BSS static char system[2048];
            err = mind_share_request_build(lang, where, fact, system, sizeof system);
            if (err == ESP_OK) err = ask_line(system, L.text, sizeof L.text, &chat, L.why, sizeof L.why);
        }
        if (!fact[0] || err != ESP_OK) {                      // a question (also the fallback when the retelling failed)
            L.question = true;
            err = mind_line_question(lang, (unsigned)(n / 2), L.text, sizeof L.text);
            if (err != ESP_OK) snprintf(L.why, sizeof L.why, "no questions for %s", lang_code(lang));
        }
        if (err == ESP_OK) store_setting_set_int("idea_n", n + 1);
    }
    const char *what = which == 2 ? "sample" : which == 0 ? "hello" : L.question ? "question" : "share";
    if (err == ESP_OK) err = synth_to_wav(what, L.text, lang, which == 2 ? s_sample_voice : NULL, L.path, which == 1 ? &chat : NULL, L.why, sizeof L.why);
    L.ok = err == ESP_OK;
    LOGI("line %s: %s (%s) %s", what, L.ok ? "ready" : "failed", L.why, L.text);
    if (cb) cb(&L, user);
}

static void request_line(int which, lang_t lang, life_line_cb done, void *user)
{
    s_line[which].lang = lang; s_line[which].user = user; s_line[which].cb = done;
    if (atomic_exchange(&s_line[which].pending, true)) return;
    if (!s_worker) { do_line(which); return; }
    struct job *pj = &s_line_job;
    queue_send(s_q, &pj, 0);
}
void life_prepare_hello(lang_t lang, life_line_cb done, void *user) { request_line(0, lang, done, user); }
void life_prepare_idea(lang_t lang, life_line_cb done, void *user) { request_line(1, lang, done, user); }
void life_voice_sample(lang_t lang, const char *voice_id, life_line_cb done, void *user)
{
    snprintf(s_sample_voice, sizeof s_sample_voice, "%s", voice_id ? voice_id : "");
    request_line(2, lang, done, user);
}

void life_check_service(unsigned roles, life_check_cb done, void *user)
{
    s_check_user = user; s_check_cb = done;
    if (atomic_fetch_or(&s_check_roles, roles ? roles : LIFE_CHECK_CHAT)) return;   // one is queued already: it asks these too
    if (!s_worker) { do_check(); return; }
    struct job *pj = &s_check_job;
    queue_send(s_q, &pj, 0);                                   // a full queue is fine: the worker looks at the flag after each job
}

static void run_job(struct job *j)
{
    switch (j->kind) {
    case JOB_PLAN: j->result = do_plan(j); break;
    case JOB_DIARY: j->result = do_diary(j); break;
    case JOB_RECAP: j->result = do_recap(j); break;
    case JOB_ART: j->result = do_art(j); break;
    default: j->result = do_tick(j); break;
    }
}

static esp_err_t submit(struct job *j)
{
    if (!s_callers) s_callers = mutex_create();
    mutex_lock(s_callers);
    s_job = *j;
    if (s_worker) {
        while (signal_wait(s_done, 0)) { }     // drop a stale latch
        struct job *pj = &s_job;
        queue_send(s_q, &pj, TASK_WAIT_FOREVER);
        signal_wait(s_done, TASK_WAIT_FOREVER);   // the worker bounds the time (vendor deadlines)
    } else run_job(&s_job);
    esp_err_t r = s_job.result;
    mutex_unlock(s_callers);
    return r;
}

static void life_task(void *arg)
{
    (void)arg;
    int64_t boot = clock_us(); bool boot_tick = false;
    for (;;) {
        struct job *j;
        if (queue_recv(s_q, &j, 60 * 1000)) {
            if (j->kind == JOB_CHECK) do_check(); else if (j->kind == JOB_LINE) { do_line(2); do_line(0); do_line(1); } else { run_job(j); signal_set(s_done); }
            do_check(); do_line(2); do_line(0); do_line(1);   // ones asked for while a long job ran
            continue;
        }
        if (!store_setting_int("auto_plan", 1)) continue;
        time_t now = world_now(); struct tm tm; localtime_r(&now, &tm);
        bool nightly = tm.tm_hour == store_setting_int("plan_hour", 3);
        bool at_boot = !boot_tick && clock_us() - boot > 120 * 1000000LL;   // a device that was off at night catches up
        if (nightly || at_boot) {
            boot_tick = true;
            char why[160];
            struct job tj = { .kind = JOB_TICK, .why = why, .why_cap = sizeof why };
            mutex_lock(s_callers);
            run_job(&tj);
            mutex_unlock(s_callers);
            LOGI("auto tick: %s (%s)", esp_err_to_name(tj.result), why);
        }
    }
}

esp_err_t life_init(void)
{
    if (!s_callers) s_callers = mutex_create();
    if (!s_q) s_q = queue_create(sizeof(struct job *), 2);
    if (!s_done) s_done = signal_create();
    if (!task_spawn("gugu_life", life_task, NULL, 12288, 2, 0)) return ESP_FAIL;
    s_worker = true;
    LOGI("life ready: auto plan %s at %02d:00", store_setting_int("auto_plan", 1) ? "on" : "off", store_setting_int("plan_hour", 3));
    return ESP_OK;
}

esp_err_t life_plan_trip(const life_plan_t *p, char *why, size_t why_cap)
{
    struct job j = { .kind = JOB_PLAN, .why = why, .why_cap = why_cap };
    if (p) {
        if (p->kind_hint) snprintf(j.kind_hint, sizeof j.kind_hint, "%s", p->kind_hint);
        if (p->place_hint) snprintf(j.place_hint, sizeof j.place_hint, "%s", p->place_hint);
        if (p->depart_date) snprintf(j.depart, sizeof j.depart, "%s", p->depart_date);
        j.nights = p->nights; j.timeout_ms = p->timeout_ms;
    }
    if (why && why_cap) why[0] = '\0';
    return submit(&j);
}

static esp_err_t simple(int kind, const char *name, char *why, size_t why_cap)
{
    char local[160];
    struct job j = { .kind = kind, .why = why ? why : local, .why_cap = why ? why_cap : sizeof local };
    j.why[0] = '\0';
    esp_err_t r = submit(&j);
    LOGI("%s: %s (%s)", name, esp_err_to_name(r), j.why);
    return r;
}

esp_err_t life_tick(void) { return simple(JOB_TICK, "tick", NULL, 0); }
esp_err_t life_diary_now(char *why, size_t why_cap) { return simple(JOB_DIARY, "diary", why, why_cap); }
esp_err_t life_recap_now(char *why, size_t why_cap) { return simple(JOB_RECAP, "recap", why, why_cap); }
esp_err_t life_art_now(char *why, size_t why_cap) { return simple(JOB_ART, "art", why, why_cap); }
