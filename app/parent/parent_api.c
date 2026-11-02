// parent_api.c — the routes, shared by the device (esp_http_server) and the host (plain sockets).
#define LOG_TAG "parent"
#include "parent_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/atomic_file.h"
#include "store/store.h"
#include "board/board.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

static const char *CT_JSON = "application/json; charset=utf-8", *CT_HTML = "text/html; charset=utf-8", *CT_TEXT = "text/plain; charset=utf-8";

static int reply(char **out, size_t *len, const char *s) { *out = strdup(s); *len = strlen(s); return 200; }

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
    if (get && !strcmp(path, "/api/status")) {
        char s[320];
        snprintf(s, sizeof s, "{\"device\":\"gugu\",\"battery_mv\":%d,\"charging\":%s,\"internal_free\":%u,\"big_free\":%u}",
                 board_battery_mv(), board_is_charging() ? "true" : "false", (unsigned)mem_avail_internal(), (unsigned)mem_avail_big());
        return reply(out, out_len, s);
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
    if (post && !strcmp(path, "/api/secret")) {            // {"key":"wifi_ssid","value":"..."} — never echoed back
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
    return reply(out, out_len, "{\"error\":\"not found\"}"), 404;
}
