// parent_api.c — the routes, shared by the device (esp_http_server) and the host (plain sockets).
#define LOG_TAG "parent"
#include "parent_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/atomic_file.h"
#include "store/store.h"
#include "board/board.h"
#include "net/net.h"
#include "foundation/clock.h"
#include "cJSON.h"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

static const char *CT_JSON = "application/json; charset=utf-8", *CT_HTML = "text/html; charset=utf-8", *CT_TEXT = "text/plain; charset=utf-8";

static int reply(char **out, size_t *len, const char *s) { *out = strdup(s); *len = strlen(s); return 200; }

static parent_hooks_t s_hooks;
void parent_set_hooks(const parent_hooks_t *h) { s_hooks = h ? *h : (parent_hooks_t){ 0 }; pair_set_card(s_hooks.card, s_hooks.user); }

static void on_join(const char *state, void *user) { pair_join(state); if (s_hooks.wifi_result) s_hooks.wifi_result(state, user); }   // the screen, then app

static void query_get(const char *query, const char *key, char *val, size_t cap)
{
    val[0] = '\0';
    if (!query) return;
    size_t kl = strlen(key);
    for (const char *p = query; *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *e = strchr(p + kl + 1, '&'); size_t n = e ? (size_t)(e - p - kl - 1) : strlen(p + kl + 1);
            if (n >= cap) n = cap - 1;
            memcpy(val, p + kl + 1, n); val[n] = '\0';
            return;
        }
        const char *amp = strchr(p, '&'); if (!amp) return; p = amp + 1;
    }
}

// JSONL file → JSON array text
static char *jsonl_to_array(const char *path)
{
    size_t n; char *txt = file_read_all(path, &n);
    strbuf_t b; strbuf_init(&b); strbuf_putc(&b, '[');
    if (txt) {
        bool first = true;
        for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
            if (!*line) continue;
            if (!first) strbuf_putc(&b, ',');
            strbuf_puts(&b, line); first = false;
        }
        mem_release(txt);
    }
    strbuf_putc(&b, ']');
    char *s = strdup(b.s ? b.s : "[]"); strbuf_free(&b);
    return s;
}

static bool safe_rel(const char *rel) { return rel && *rel && !strstr(rel, "..") && rel[0] != '/'; }

// A story's file name as the shelf writes it: 20261106-143345-00.json, nothing else.
static bool story_name_ok(const char *n)
{
    size_t l = strlen(n);
    if (l < 10 || l > 40 || strcmp(n + l - 5, ".json")) return false;
    for (size_t i = 0; i < l - 5; i++) if (!((n[i] >= '0' && n[i] <= '9') || n[i] == '-')) return false;
    return true;
}
static int name_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }
// [{"name":…,"t":…,"lang":…,"title":…,"chars":…}], newest first (the text itself stays on the device until asked for)
static char *stories_list(const char *dir)
{
    char (*names)[48] = malloc(sizeof *names * 64); int n = 0;
    DIR *d = names ? opendir(dir) : NULL;
    for (struct dirent *e; d && (e = readdir(d)) && n < 64;) if (story_name_ok(e->d_name)) snprintf(names[n++], 48, "%.47s", e->d_name);
    if (d) closedir(d);
    if (n) qsort(names, (size_t)n, sizeof *names, name_desc);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        char path[240]; snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        char *txt = file_read_all(path, NULL);
        cJSON *st = txt ? cJSON_Parse(txt) : NULL;
        if (txt) mem_release(txt);
        if (!st) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", names[i]);
        const char *keys[] = { "t", "lang", "title" };
        for (int k = 0; k < 3; k++) { const cJSON *v = cJSON_GetObjectItem(st, keys[k]); cJSON_AddStringToObject(o, keys[k], cJSON_IsString(v) ? v->valuestring : ""); }
        const cJSON *tx = cJSON_GetObjectItem(st, "text");
        size_t chars = 0; if (cJSON_IsString(tx)) for (const char *q = tx->valuestring; *q; q++) if (((unsigned char)*q & 0xC0) != 0x80) chars++;
        cJSON_AddNumberToObject(o, "chars", (double)chars);
        cJSON_AddItemToArray(arr, o);
        cJSON_Delete(st);
    }
    free(names);
    char *json = cJSON_PrintUnformatted(arr); cJSON_Delete(arr);
    return json ? json : strdup("[]");
}

int parent_api_handle(const char *method, const char *path, const char *query, const char *body, size_t body_len,
                      char **out, size_t *out_len, const char **ctype, const char **disposition)
{
    *ctype = CT_JSON; *disposition = NULL; *out = NULL; *out_len = 0;
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (parent_is_active()) pair_page();                  // the page is open on a phone: the screen moves on from the codes
    char p[200];
    if (get && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
        snprintf(p, sizeof p, "%s/web/index.html", store_content_dir());
        size_t n; char *s = file_read_all(p, &n);
        if (!s) return reply(out, out_len, "<h1>gugu</h1><p>content/web/index.html missing</p>"), *ctype = CT_HTML, 200;
        *out = strdup(s); *out_len = n; mem_release(s); *ctype = CT_HTML; return 200;
    }
    if (get && !strcmp(path, "/api/status")) {           // live numbers plus what app.c recorded at boot (logs/boot.json: version, audio, wifi, key)
        snprintf(p, sizeof p, "%s/logs/boot.json", store_data_dir());
        size_t bn; char *boot = file_read_all(p, &bn);
        cJSON *j = boot ? cJSON_Parse(boot) : NULL; mem_release(boot);
        if (!j) j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "device", "gugu");
        cJSON_AddNumberToObject(j, "battery_mv", board_battery_mv());
        cJSON_AddBoolToObject(j, "charging", board_is_charging());
        cJSON_AddBoolToObject(j, "online", net_is_online());
        char ssid[33] = "";                               // boot.json has it as of the boot; the page may have just connected
        store_secret_get("wifi_ssid", ssid, sizeof ssid);
        cJSON_DeleteItemFromObject(j, "wifi_configured");
        cJSON_AddBoolToObject(j, "wifi_configured", ssid[0] != '\0');
        if (s_hooks.problem) {                            // can a call start, and if not, what to fix: the page's banner
            parent_problem_t pr = { .ready = true };
            s_hooks.problem(&pr, s_hooks.user);
            cJSON_AddBoolToObject(j, "ready", pr.ready);
            if (!pr.ready) {
                cJSON *o = cJSON_AddObjectToObject(j, "problem");
                cJSON_AddStringToObject(o, "cause", pr.cause); cJSON_AddStringToObject(o, "role", pr.role);
                cJSON_AddStringToObject(o, "vendor", pr.vendor); cJSON_AddStringToObject(o, "why", pr.why);
            }
        }
        cJSON_AddNumberToObject(j, "internal_free", (double)mem_avail_internal());
        cJSON_AddNumberToObject(j, "big_free", (double)mem_avail_big());
        char *txt = cJSON_PrintUnformatted(j); cJSON_Delete(j);
        *out = strdup(txt ? txt : "{}"); *out_len = strlen(*out); cJSON_free(txt); return 200;
    }
    if (get && !strcmp(path, "/api/logs")) {             // one text file for diagnosis: live status, then store's bundle (secrets masked)
        size_t n = 0; char *body = store_export_logs(&n);
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        strbuf_t b; strbuf_init(&b);
        strbuf_printf(&b, "gugu logs, exported %04d-%02d-%02d %02d:%02d:%02d local, uptime %u s\n", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100, tm.tm_mday % 100,
                      tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100, (unsigned)(clock_ms() / 1000));
        strbuf_printf(&b, "online %s, battery %d mV%s, internal RAM free %u B (lowest %u B), PSRAM free %u B, pairing mode %s\n",
                      net_is_online() ? "yes" : "no", board_battery_mv(), board_is_charging() ? " charging" : "", (unsigned)mem_avail_internal(),
                      (unsigned)mem_min_avail_internal(), (unsigned)mem_avail_big(), parent_is_active() ? "on" : "off");
        if (body) { strbuf_append(&b, body, n); mem_release(body); }
        *out_len = b.len; *out = malloc(b.len + 1);
        if (*out) memcpy(*out, b.s ? b.s : "", b.len + 1); else *out_len = 0;
        strbuf_free(&b);
        static char disp[96];
        snprintf(disp, sizeof disp, "attachment; filename=\"gugu-logs-%04d%02d%02d-%02d%02d.txt\"", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100,
                 tm.tm_mday % 100, tm.tm_hour % 100, tm.tm_min % 100);
        char view[8]; query_get(query, "view", view, sizeof view);
        *ctype = CT_TEXT; *disposition = view[0] ? NULL : disp;
        return 200;
    }
    if (post && !strcmp(path, "/api/test/audio")) {      // speaker → microphone loopback, levels as JSON
        char json[256] = "{\"error\":\"not available\"}";
        if (s_hooks.test_audio) s_hooks.test_audio(json, sizeof json, s_hooks.user);
        return reply(out, out_len, json);
    }
    if (post && !strcmp(path, "/api/apply")) {           // after saving settings or keys: use them now
        if (s_hooks.apply) s_hooks.apply(s_hooks.user);
        return reply(out, out_len, "{\"ok\":true}");
    }
    if (post && !strcmp(path, "/api/volume")) {          // the speaker section's slider: kept, and in use at once
        cJSON *j = body ? cJSON_ParseWithLength(body, body_len) : NULL;
        cJSON *v = cJSON_GetObjectItem(j, "volume");
        bool ok = cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 100;
        int vol = ok ? (int)v->valuedouble : 0;
        cJSON_Delete(j);
        if (!ok) return reply(out, out_len, "{\"error\":\"volume is 0 to 100\"}"), 400;
        store_setting_set_int("volume", vol);
        if (s_hooks.volume) s_hooks.volume(vol, s_hooks.user);
        return reply(out, out_len, "{\"ok\":true}");
    }
    if (post && !strcmp(path, "/api/test/sound")) {      // the owner taps "test the speaker"
        if (s_hooks.test_sound) s_hooks.test_sound(s_hooks.user);
        return reply(out, out_len, s_hooks.test_sound ? "{\"ok\":true}" : "{\"ok\":false}");
    }
    if (get && !strcmp(path, "/api/calls")) {
        snprintf(p, sizeof p, "%s/calls/index.jsonl", store_data_dir());
        *out = jsonl_to_array(p); *out_len = strlen(*out); return 200;
    }
    // The story shelf (data/stories/, mind_story.c): the list newest first, one story whole, and forgetting one.
    if (get && !strcmp(path, "/api/stories")) {
        snprintf(p, sizeof p, "%s/stories", store_data_dir());
        *out = stories_list(p); *out_len = strlen(*out); return 200;
    }
    if (get && !strcmp(path, "/api/story")) {
        char name[48]; query_get(query, "name", name, sizeof name);
        if (!story_name_ok(name)) return reply(out, out_len, "{\"error\":\"bad name\"}"), 400;
        snprintf(p, sizeof p, "%s/stories/%s", store_data_dir(), name);
        size_t n; char *s = file_read_all(p, &n);
        if (!s) return reply(out, out_len, "{\"error\":\"no such story\"}"), 404;
        *out = strdup(s); *out_len = n; mem_release(s); return 200;
    }
    if (post && !strcmp(path, "/api/story/forget")) {        // {"name":"20261106-143345-00.json"}
        cJSON *j = cJSON_ParseWithLength(body, body_len);
        const cJSON *nm = j ? cJSON_GetObjectItem(j, "name") : NULL;
        char name[48] = ""; if (cJSON_IsString(nm)) snprintf(name, sizeof name, "%s", nm->valuestring);
        cJSON_Delete(j);
        if (!story_name_ok(name)) return reply(out, out_len, "{\"error\":\"bad name\"}"), 400;
        snprintf(p, sizeof p, "%s/stories/%s", store_data_dir(), name);
        bool ok = remove(p) == 0;
        store_event("story forgotten from the page: %s", name);
        return reply(out, out_len, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    }
    if (get && !strcmp(path, "/api/call")) {
        char rel[160]; query_get(query, "file", rel, sizeof rel);
        if (!safe_rel(rel)) return reply(out, out_len, "{\"error\":\"bad file\"}"), 400;
        snprintf(p, sizeof p, "%s/%s", store_data_dir(), rel);
        *out = jsonl_to_array(p); *out_len = strlen(*out); return 200;
    }
    if (get && !strcmp(path, "/api/settings")) {
        snprintf(p, sizeof p, "%s/settings.json", store_data_dir());
        size_t n; char *s = file_read_all(p, &n);
        if (!s) return reply(out, out_len, "{}");
        *out = strdup(s); *out_len = n; mem_release(s); return 200;
    }
    if (post && !strcmp(path, "/api/settings")) {
        cJSON *j = body ? cJSON_ParseWithLength(body, body_len) : NULL;
        if (!j) return reply(out, out_len, "{\"error\":\"bad json\"}"), 400;
        for (cJSON *it = j->child; it; it = it->next) {
            if (cJSON_IsNumber(it)) store_setting_set_int(it->string, (int)it->valuedouble);
            else if (cJSON_IsString(it)) store_setting_set_str(it->string, it->valuestring);
        }
        cJSON_Delete(j);
        return reply(out, out_len, "{\"ok\":true}");
    }
    if (get && !strcmp(path, "/api/events")) {             // the last lines of the event log, for a report
        snprintf(p, sizeof p, "%s/logs/events.log", store_data_dir());
        size_t n; char *s = file_read_all(p, &n);
        if (!s) return reply(out, out_len, ""), *ctype = CT_TEXT, 200;
        const char *tail = n > 6000 ? s + n - 6000 : s;
        *out = strdup(tail); *out_len = strlen(tail); mem_release(s); *ctype = CT_TEXT; return 200;
    }
    if (get && !strcmp(path, "/api/wifi/scan")) {          // nearby networks for the pairing page's picker: the list so far; sweep=1 starts another sweep in the background
        static MEM_BIG_BSS net_wifi_ap_t aps[48];          // the server handles one request at a time
        char sweep[4]; query_get(query, "sweep", sweep, sizeof sweep);
        if (sweep[0] == '1') net_wifi_sweep();
        bool sweeping = false; int n = net_wifi_nearby(aps, 48, &sweeping);
        strbuf_t b; strbuf_init(&b); strbuf_printf(&b, "{\"sweeping\":%s,\"aps\":[", sweeping ? "true" : "false");
        for (int i = 0; i < n; i++) { strbuf_printf(&b, "%s{\"ssid\":\"", i ? "," : ""); strbuf_append_json_escaped(&b, aps[i].ssid); strbuf_printf(&b, "\",\"rssi\":%d,\"secure\":%s}", aps[i].rssi, aps[i].secure ? "true" : "false"); }
        strbuf_puts(&b, "]}");
        *out = strdup(b.s ? b.s : "{}"); *out_len = strlen(*out); strbuf_free(&b); return 200;
    }
    if (post && !strcmp(path, "/api/wifi/join")) {         // {"ssid":"…","pass":"…"}: tried now, kept only if it works; GET says how it went
        cJSON *j = body ? cJSON_ParseWithLength(body, body_len) : NULL;
        const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid")), *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));
        size_t sl = ssid ? strlen(ssid) : 0, pl = pass ? strlen(pass) : 0;
        if (sl < 1 || sl > 32 || (pl && (pl < 8 || pl > 63))) { cJSON_Delete(j); return reply(out, out_len, "{\"error\":\"a name of 1 to 32 bytes; no password, or 8 to 63\"}"), 400; }
        bool started = parent_join_start(ssid, pass ? pass : "", on_join, s_hooks.user);
        cJSON_Delete(j);
        return reply(out, out_len, started ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"busy\"}");
    }
    if (get && !strcmp(path, "/api/wifi/join")) {
        char json[48]; snprintf(json, sizeof json, "{\"state\":\"%s\"}", parent_join_state());
        return reply(out, out_len, json);
    }
    if (get && !strcmp(path, "/api/secrets")) {            // the owner's own Wi-Fi and keys, shown on the owner's own page (the hotspot is the lock)
        strbuf_t b; strbuf_init(&b); strbuf_putc(&b, '{');
        for (int i = 0; store_secret_name(i); i++) {      // Wi-Fi and every key a provider preset declared
            char v[256]; if (store_secret_get(store_secret_name(i), v, sizeof v) != ESP_OK) v[0] = '\0';
            strbuf_printf(&b, "%s\"%s\":\"", i ? "," : "", store_secret_name(i)); strbuf_append_json_escaped(&b, v); strbuf_putc(&b, '"');
        }
        strbuf_putc(&b, '}');
        *out = strdup(b.s ? b.s : "{}"); *out_len = strlen(*out); strbuf_free(&b); return 200;
    }
    if (get && !strcmp(path, "/api/providers")) {          // the presets as they are (content/providers/*.json): roles, regions, keys, voices
        snprintf(p, sizeof p, "%s/providers", store_content_dir());
        DIR *d = opendir(p);
        strbuf_t b; strbuf_init(&b); strbuf_putc(&b, '[');
        bool first = true;
        for (struct dirent *e = d ? readdir(d) : NULL; e; e = readdir(d)) {
            size_t n = strlen(e->d_name);
            if (n < 6 || strcmp(e->d_name + n - 5, ".json")) continue;
            char fp[240]; snprintf(fp, sizeof fp, "%s/%s", p, e->d_name);
            size_t fl; char *txt = file_read_all(fp, &fl);
            if (!txt) continue;
            if (!first) strbuf_putc(&b, ',');
            strbuf_append(&b, txt, fl); first = false;
            mem_release(txt);
        }
        if (d) closedir(d);
        strbuf_putc(&b, ']');
        *out = strdup(b.s ? b.s : "[]"); *out_len = strlen(*out); strbuf_free(&b); return 200;
    }
    if (post && !strcmp(path, "/api/check")) {             // after saving: do the vendors answer? (a tiny chat, a word of voice, a moment of silence heard)
        char json[400] = "{\"ok\":false,\"why\":\"not wired\"}";
        if (s_hooks.check) s_hooks.check(json, sizeof json, s_hooks.user);
        return reply(out, out_len, json);
    }
    if (post && !strcmp(path, "/api/voice/test")) {        // {"lang":"yue","voice":"Cantonese_CuteGirl"}: Gugu says the preview line in it
        cJSON *j = body ? cJSON_ParseWithLength(body, body_len) : NULL;
        const char *lang = cJSON_GetStringValue(cJSON_GetObjectItem(j, "lang")), *voice = cJSON_GetStringValue(cJSON_GetObjectItem(j, "voice"));
        char json[400] = "{\"ok\":false,\"why\":\"not wired\"}";
        if (s_hooks.voice_test) s_hooks.voice_test(lang ? lang : "", voice ? voice : "", json, sizeof json, s_hooks.user);
        cJSON_Delete(j);
        return reply(out, out_len, json);
    }
    if (post && !strcmp(path, "/api/secret")) {            // {"key":"wifi_ssid","value":"..."}
        cJSON *j = body ? cJSON_ParseWithLength(body, body_len) : NULL;
        const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(j, "key")), *v = cJSON_GetStringValue(cJSON_GetObjectItem(j, "value"));
        if (!k || !v) { cJSON_Delete(j); return reply(out, out_len, "{\"error\":\"key and value required\"}"), 400; }
        esp_err_t err = store_secret_set(k, v);
        cJSON_Delete(j);
        return reply(out, out_len, err == ESP_OK ? "{\"ok\":true}" : "{\"error\":\"store failed\"}");
    }
    if (get && !strcmp(path, "/api/export")) {             // every call as one JSONL download
        strbuf_t b; strbuf_init(&b);
        snprintf(p, sizeof p, "%s/calls", store_data_dir());
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.' || strchr(e->d_name, '.')) continue;   // month directories only
            char mdir[260]; snprintf(mdir, sizeof mdir, "%.200s/%.48s", p, e->d_name);
            DIR *md = opendir(mdir); struct dirent *f;
            while (md && (f = readdir(md)) != NULL) {
                if (!strstr(f->d_name, ".jsonl")) continue;
                char fp[400]; snprintf(fp, sizeof fp, "%.260s/%.64s", mdir, f->d_name);
                size_t n; char *txt = file_read_all(fp, &n);
                if (txt) { strbuf_printf(&b, "# %s/%s\n", e->d_name, f->d_name); strbuf_append(&b, txt, n); strbuf_putc(&b, '\n'); mem_release(txt); }
            }
            if (md) closedir(md);
        }
        if (d) closedir(d);
        *out = strdup(b.s ? b.s : ""); *out_len = b.len; strbuf_free(&b);
        *ctype = CT_TEXT; *disposition = "attachment; filename=\"gugu-calls.jsonl\"";
        return 200;
    }
    if (get && !strcmp(path, "/api/trip")) {               // the current trip card (data/trips/current.json), or null
        snprintf(p, sizeof p, "%s/trips/current.json", store_data_dir());
        size_t n; char *s = file_read_all(p, &n);
        if (!s) return reply(out, out_len, "null");
        *out = strdup(s); *out_len = n; mem_release(s); return 200;
    }
    if (get && !strcmp(path, "/api/trips")) {              // every card ever planned, one row each
        snprintf(p, sizeof p, "%s/trips/index.jsonl", store_data_dir());
        *out = jsonl_to_array(p); *out_len = strlen(*out); return 200;
    }
    if (get && !strcmp(path, "/api/memory")) {             // ?book=journey|about_child|canon_grown
        char book[24]; query_get(query, "book", book, sizeof book);
        if (!strcmp(book, "journey")) { snprintf(p, sizeof p, "%s/memory/journey.jsonl", store_data_dir()); *out = jsonl_to_array(p); *out_len = strlen(*out); return 200; }
        if (strcmp(book, "about_child") && strcmp(book, "canon_grown")) return reply(out, out_len, "{\"error\":\"unknown book\"}"), 400;
        snprintf(p, sizeof p, "%s/memory/%s.json", store_data_dir(), book);
        size_t n; char *s = file_read_all(p, &n);
        if (!s) return reply(out, out_len, "[]");
        *out = strdup(s); *out_len = n; mem_release(s); return 200;
    }
    if (post && !strcmp(path, "/api/memory/forget")) {     // {"book":"journey","key":"<trip id>"} or {"book":"about_child","key":"<index>"}
        cJSON *j = body ? cJSON_ParseWithLength(body, body_len) : NULL;
        const char *book = cJSON_GetStringValue(cJSON_GetObjectItem(j, "book")), *key = cJSON_GetStringValue(cJSON_GetObjectItem(j, "key"));
        if (!book || !key) { cJSON_Delete(j); return reply(out, out_len, "{\"error\":\"book and key required\"}"), 400; }
        if (s_hooks.memory_forget) s_hooks.memory_forget(book, key, s_hooks.user);
        cJSON_Delete(j);
        return reply(out, out_len, "{\"ok\":true}");
    }
    if (post && !strcmp(path, "/api/trip/delete")) {       // the parent cancels the trip; Gugu forgets it
        if (s_hooks.trip_delete) s_hooks.trip_delete(s_hooks.user);
        else { snprintf(p, sizeof p, "%s/trips/current.json", store_data_dir()); remove(p); }
        return reply(out, out_len, "{\"ok\":true}");
    }
    return reply(out, out_len, "{\"error\":\"not found\"}"), 404;
}
