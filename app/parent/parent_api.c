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
void parent_set_hooks(const parent_hooks_t *h) { s_hooks = h ? *h : (parent_hooks_t){ 0 }; }

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

int parent_api_handle(const char *method, const char *path, const char *query, const char *body, size_t body_len,
                      char **out, size_t *out_len, const char **ctype, const char **disposition)
{
    *ctype = CT_JSON; *disposition = NULL; *out = NULL; *out_len = 0;
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
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
    if (post && !strcmp(path, "/api/apply")) {           // {"wifi":true} after saving secrets: use them now
        cJSON *j = body ? cJSON_ParseWithLength(body, body_len) : NULL;
        bool wifi = cJSON_IsTrue(cJSON_GetObjectItem(j, "wifi"));
        cJSON_Delete(j);
        if (s_hooks.apply) s_hooks.apply(wifi, s_hooks.user);
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
    if (get && !strcmp(path, "/api/wifi/scan")) {          // nearby networks for the pairing page's picker
        net_wifi_ap_t aps[16]; int n = net_wifi_scan(aps, 16);
        strbuf_t b; strbuf_init(&b); strbuf_putc(&b, '[');
        for (int i = 0; i < n; i++) { strbuf_printf(&b, "%s{\"ssid\":\"", i ? "," : ""); strbuf_append_json_escaped(&b, aps[i].ssid); strbuf_printf(&b, "\",\"rssi\":%d,\"secure\":%s}", aps[i].rssi, aps[i].secure ? "true" : "false"); }
        strbuf_putc(&b, ']');
        *out = strdup(b.s ? b.s : "[]"); *out_len = strlen(*out); strbuf_free(&b); return 200;
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
