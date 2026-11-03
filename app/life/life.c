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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { JOB_PLAN = 1, JOB_TICK, JOB_DIARY, JOB_RECAP, JOB_ART };
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
struct collect { strbuf_t text; bool failed; provider_error_t err; char stop[24]; uint32_t out_tokens; };
static void c_delta(const char *t, size_t n, void *u) { strbuf_append(&((struct collect *)u)->text, t, n); }
static void c_done(const char *stop, const provider_usage_t *us, void *u) { struct collect *c = u; snprintf(c->stop, sizeof c->stop, "%s", stop ? stop : ""); c->out_tokens = us ? us->out_tokens : 0; }
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
        if (queue_recv(s_q, &j, 60 * 1000)) { run_job(j); signal_set(s_done); continue; }
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
