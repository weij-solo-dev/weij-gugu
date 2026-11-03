// world.c — the card on flash, the phase from the clock, the time of day where Gugu is.
#define LOG_TAG "world"
#include "world/world.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/task.h"
#include "foundation/strbuf.h"
#include "foundation/atomic_file.h"
#include "store/store.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static char s_dir[128];
static mutex_t s_lock;
MEM_BIG_BSS static world_trip_t s_trip;
static bool s_have_trip;
static int64_t s_clock_offset;   // seconds added to time(NULL): the console and the tests walk a trip in minutes

static void ensure(void) { if (!s_lock) s_lock = mutex_create(); }

// ---- clock and dates ----
time_t  world_now(void) { return time(NULL) + (time_t)s_clock_offset; }
void    world_clock_shift(int64_t seconds) { s_clock_offset += seconds; }
void    world_clock_set(time_t now) { s_clock_offset = (int64_t)now - (int64_t)time(NULL); }
void    world_clock_reset(void) { s_clock_offset = 0; }
int64_t world_clock_offset(void) { return s_clock_offset; }

static bool parse_ymd(const char *s, int *y, int *m, int *d)
{
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    *y = atoi(s); *m = atoi(s + 5); *d = atoi(s + 8);
    return *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31;
}

static time_t ymd_at(const char *ymd, int hour)   // home-local calendar date at hour:00
{
    int y, m, d;
    if (!parse_ymd(ymd, &y, &m, &d)) return 0;
    struct tm tm = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = hour, .tm_isdst = -1 };
    return mktime(&tm);
}

static void fmt_ymd(time_t t, char *out, size_t cap)
{
    struct tm tm; localtime_r(&t, &tm);
    snprintf(out, cap, "%04u-%02u-%02u", (unsigned)(tm.tm_year + 1900) % 10000u, (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u);
}

void world_today(char *out, size_t cap) { fmt_ymd(world_now(), out, cap); }
void world_date_add(const char *ymd, int days, char *out, size_t cap) { fmt_ymd(ymd_at(ymd, 12) + (time_t)days * 86400, out, cap); }
int  world_date_diff(const char *from, const char *to) { return (int)lround(difftime(ymd_at(to, 12), ymd_at(from, 12)) / 86400.0); }

// The home zone is whatever TZ store set (settings "tz"), measured rather than parsed.
static int home_tz_offset_min(time_t now)
{
    struct tm g; gmtime_r(&now, &g); g.tm_isdst = -1;
    return (int)(difftime(now, mktime(&g)) / 60.0);
}

void world_home(world_place_t *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", store_setting_str("home_city", "广州"));
    snprintf(out->country, sizeof out->country, "%s", store_setting_str("home_country", "中国"));
    out->lat = atof(store_setting_str("home_lat", "23.13"));
    out->lon = atof(store_setting_str("home_lon", "113.26"));
    out->tz_offset_min = home_tz_offset_min(world_now());
}

// ---- the card ----
static void path_current(char *p, size_t cap) { snprintf(p, cap, "%s/trips/current.json", s_dir); }

esp_err_t world_init(const char *data_dir)
{
    ensure();
    snprintf(s_dir, sizeof s_dir, "%s", data_dir);
    char p[200]; path_current(p, sizeof p);
    size_t n; char *txt = file_read_all(p, &n);
    mutex_lock(s_lock);
    s_have_trip = false;
    if (txt) {
        char why[96];
        if (world_trip_from_json(txt, n, &s_trip, why, sizeof why)) s_have_trip = true;
        else LOGW("trips/current.json ignored: %s", why);
        mem_release(txt);
    }
    mutex_unlock(s_lock);
    world_state_t st; world_state(&st);
    LOGI("world ready: trip %s%s%s, %s %s (%02d:%02d, sun %02d:%02d-%02d:%02d)", world_phase_name(st.phase),
         s_have_trip ? " to " : "", s_have_trip ? s_trip.place.name : "", st.place, world_tod_name(st.tod), st.local_hour, st.local_min,
         st.sunrise_min / 60, st.sunrise_min % 60, st.sunset_min / 60, st.sunset_min % 60);
    return ESP_OK;
}

const world_trip_t *world_trip_lock(void)
{
    ensure();
    mutex_lock(s_lock);
    if (s_have_trip) return &s_trip;
    mutex_unlock(s_lock);
    return NULL;
}

void world_trip_unlock(void) { mutex_unlock(s_lock); }

esp_err_t world_trip_set(const world_trip_t *t)
{
    ensure();
    char *txt = world_trip_to_json(t);
    if (!txt) return ESP_ERR_NO_MEM;
    char p[200]; path_current(p, sizeof p);
    bool ok = atomic_file_write(p, txt, strlen(txt));
    if (ok && t->id[0]) { snprintf(p, sizeof p, "%s/trips/%s.json", s_dir, t->id); atomic_file_write(p, txt, strlen(txt)); }
    mem_release(txt);
    if (!ok) return ESP_FAIL;
    strbuf_t b; strbuf_init(&b);
    strbuf_printf(&b, "{\"id\":\"%s\",\"kind\":\"%s\",\"place\":\"", t->id, world_kind_name(t->kind));
    strbuf_append_json_escaped(&b, t->place.name);
    strbuf_puts(&b, "\",\"country\":\"");
    strbuf_append_json_escaped(&b, t->place.country);
    strbuf_printf(&b, "\",\"depart\":\"%s\",\"return\":\"%s\",\"nights\":%d,\"generated_at\":\"%s\"}\n", t->depart_date, t->return_date, t->nights, t->generated_at);
    snprintf(p, sizeof p, "%s/trips/index.jsonl", s_dir);
    if (b.s) file_append(p, b.s, b.len);
    strbuf_free(&b);
    mutex_lock(s_lock);
    s_trip = *t; s_have_trip = true;
    mutex_unlock(s_lock);
    LOGI("trip set: %s %s to %s, %s..%s (%d nights)", t->id, world_kind_name(t->kind), t->place.name, t->depart_date, t->return_date, t->nights);
    return ESP_OK;
}

static esp_err_t write_current(void)   // s_lock held
{
    char *txt = world_trip_to_json(&s_trip);
    if (!txt) return ESP_ERR_NO_MEM;
    char p[200]; path_current(p, sizeof p);
    bool ok = atomic_file_write(p, txt, strlen(txt));
    if (ok && s_trip.id[0]) { snprintf(p, sizeof p, "%s/trips/%s.json", s_dir, s_trip.id); atomic_file_write(p, txt, strlen(txt)); }
    mem_release(txt);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t world_trip_set_recap(const char *recap, const char (*open_hooks)[160], int n)
{
    ensure();
    mutex_lock(s_lock);
    if (!s_have_trip) { mutex_unlock(s_lock); return ESP_ERR_NOT_FOUND; }
    snprintf(s_trip.recap, sizeof s_trip.recap, "%s", recap ? recap : "");
    s_trip.hook_count = 0;
    for (int i = 0; i < n && i < 2; i++) if (open_hooks[i][0]) snprintf(s_trip.hooks[s_trip.hook_count++], sizeof s_trip.hooks[0], "%s", open_hooks[i]);
    esp_err_t err = write_current();
    mutex_unlock(s_lock);
    return err;
}

esp_err_t world_trip_forget(const char *id)
{
    ensure();
    if (!id || !*id || strstr(id, "..") || strchr(id, '/')) return ESP_ERR_INVALID_ARG;
    char p[200]; snprintf(p, sizeof p, "%s/trips/%s.json", s_dir, id);
    if (file_exists(p)) remove(p);
    mutex_lock(s_lock);
    bool current = s_have_trip && !strcmp(s_trip.id, id);
    mutex_unlock(s_lock);
    if (current) return world_trip_clear();
    LOGI("trip %s forgotten", id);
    return ESP_OK;
}

esp_err_t world_trip_clear(void)
{
    ensure();
    mutex_lock(s_lock);
    s_have_trip = false;
    mutex_unlock(s_lock);
    char p[200]; path_current(p, sizeof p);
    if (file_exists(p) && remove(p) != 0) return ESP_FAIL;
    LOGI("trip cleared");
    return ESP_OK;
}

void world_recent_places(char *out, size_t cap)
{
    out[0] = '\0';
    char p[200]; snprintf(p, sizeof p, "%s/trips/index.jsonl", s_dir);
    size_t n; char *txt = file_read_all(p, &n);
    if (!txt) return;
    const char *names[6]; int count = 0;
    char *ring[64]; int total = 0;                  // the last 64 lines, newest last
    for (char *l = strtok(txt, "\n"); l; l = strtok(NULL, "\n")) if (*l) ring[total++ % 64] = l;
    int nl = total < 64 ? total : 64;
    size_t o = 0;
    for (int k = 1; k <= nl && count < 6; k++) {
        cJSON *j = cJSON_Parse(ring[(total - k) % 64]);
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "place")), *country = cJSON_GetStringValue(cJSON_GetObjectItem(j, "country"));
        if (name && *name) {
            bool dup = false;
            for (int k = 0; k < count; k++) if (!strcmp(names[k], name)) dup = true;
            if (!dup) {
                int w = country && *country ? snprintf(out + o, o < cap ? cap - o : 0, "%s%s（%s）", count ? "、" : "", name, country)
                                            : snprintf(out + o, o < cap ? cap - o : 0, "%s%s", count ? "、" : "", name);
                if (w > 0) o += (size_t)w;
                names[count++] = name;      // points into txt, alive until the release below
            }
        }
        cJSON_Delete(j);                    // name was copied into out already
    }
    mem_release(txt);
}

// ---- the state ----
void world_state(world_state_t *out)
{
    ensure();
    memset(out, 0, sizeof *out);
    time_t now = world_now();
    out->now = now;
    world_place_t where; world_home(&where);
    int depart_hour = store_setting_int("depart_hour", 8), return_hour = store_setting_int("return_hour", 17);
    mutex_lock(s_lock);
    if (s_have_trip) {
        time_t dep = ymd_at(s_trip.depart_date, depart_hour), ret = ymd_at(s_trip.return_date, return_hour);
        char today[11]; fmt_ymd(now, today, sizeof today);
        int di = world_date_diff(s_trip.depart_date, today);
        if (dep == 0 || ret == 0) { out->phase = WORLD_TRIP_NONE; }
        else if (now < dep) { out->phase = WORLD_TRIP_PLANNED; out->day_index = di; out->nights_left = s_trip.nights; }
        else if (now < ret) {
            out->phase = WORLD_TRIP_AWAY; out->away = true;
            out->day_index = di < 0 ? 0 : di;
            out->nights_left = s_trip.nights - out->day_index; if (out->nights_left < 0) out->nights_left = 0;
            where = s_trip.place;
        } else { out->phase = WORLD_TRIP_DONE; out->day_index = di; }
    }
    mutex_unlock(s_lock);
    snprintf(out->place, sizeof out->place, "%s", where.name);
    time_t loc = now + (time_t)where.tz_offset_min * 60;
    struct tm tm; gmtime_r(&loc, &tm);
    out->local_hour = tm.tm_hour; out->local_min = tm.tm_min;
    if (world_sun_times(where.lat, where.lon, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, where.tz_offset_min, &out->sunrise_min, &out->sunset_min))
        out->tod = world_tod_classify(tm.tm_hour * 60 + tm.tm_min, out->sunrise_min, out->sunset_min);
    else   // polar day or night: summer on that hemisphere means day
        out->tod = ((where.lat > 0) == (tm.tm_mon >= 3 && tm.tm_mon <= 8)) ? WORLD_TOD_DAY : WORLD_TOD_NIGHT;
}
