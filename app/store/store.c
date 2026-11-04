// store.c — portable implementation on stdio (works on LittleFS and the host filesystem).
#define LOG_TAG "store"
#include "store/store.h"
#include "store_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/atomic_file.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

static char s_data[96], s_content[96];
static char s_secret_names[24][40] = { "wifi_ssid", "wifi_pass" };
static int s_secret_n = 2;

void store_secret_declare(const char *key)
{
    if (!key || !*key) return;
    for (int i = 0; i < s_secret_n; i++) if (!strcmp(s_secret_names[i], key)) return;
    if (s_secret_n < 24) snprintf(s_secret_names[s_secret_n++], sizeof s_secret_names[0], "%s", key);
}

const char *store_secret_name(int i) { return i >= 0 && i < s_secret_n ? s_secret_names[i] : NULL; }
static cJSON *s_settings;

static void mkdirs(const char *path)
{
    char tmp[200]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

esp_err_t store_init(const char *data_dir, const char *content_dir)
{
    snprintf(s_data, sizeof s_data, "%s", data_dir);
    snprintf(s_content, sizeof s_content, "%s", content_dir);
    store_mount_all();
    char sub[200];
    const char *dirs[] = { "calls", "ledger", "memory", "diary", "trips", "postcards", "souvenirs", "art", "logs", "voice", NULL };
    mkdirs(s_data);
    for (int i = 0; dirs[i]; i++) { snprintf(sub, sizeof sub, "%s/%s", s_data, dirs[i]); mkdirs(sub); }
    snprintf(sub, sizeof sub, "%s/settings.json", s_data);
    char *txt = file_read_all(sub, NULL);
    s_settings = txt ? cJSON_Parse(txt) : NULL;
    mem_release(txt);
    if (!s_settings) s_settings = cJSON_CreateObject();
    esp_err_t err = store_secret_init();
    // Dated files (calls/<yyyy-mm>, ledger, diary) use local time; the zone is a setting, China by default.
    setenv("TZ", store_setting_str("tz", "CST-8"), 1);
    tzset();
    LOGI("store ready: data=%s content=%s settings=%d keys tz=%s", s_data, s_content, cJSON_GetArraySize(s_settings), store_setting_str("tz", "CST-8"));
    return err;
}

const char *store_data_dir(void) { return s_data; }
const char *store_content_dir(void) { return s_content; }

static esp_err_t save_settings(void)
{
    char *txt = cJSON_Print(s_settings);
    if (!txt) return ESP_ERR_NO_MEM;
    char path[200]; snprintf(path, sizeof path, "%s/settings.json", s_data);
    bool ok = atomic_file_write(path, txt, strlen(txt));
    cJSON_free(txt);
    return ok ? ESP_OK : ESP_FAIL;
}

const char *store_setting_str(const char *key, const char *dflt)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(s_settings, key));
    return v ? v : dflt;
}

int store_setting_int(const char *key, int dflt)
{
    cJSON *v = cJSON_GetObjectItem(s_settings, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : dflt;
}

esp_err_t store_setting_set_str(const char *key, const char *value)
{
    cJSON_DeleteItemFromObject(s_settings, key);
    cJSON_AddStringToObject(s_settings, key, value);
    return save_settings();
}

esp_err_t store_setting_set_int(const char *key, int value)
{
    cJSON_DeleteItemFromObject(s_settings, key);
    cJSON_AddNumberToObject(s_settings, key, value);
    return save_settings();
}

// ---- call log ----
static void month_dir(char *out, size_t cap, const char *sub, struct tm *tm)
{
    snprintf(out, cap, "%.96s/%.16s/%04u-%02u", s_data, sub, (unsigned)(tm->tm_year + 1900) % 10000u, (unsigned)(tm->tm_mon + 1) % 100u);
    mkdirs(out);
}

esp_err_t store_call_begin(store_call_t *c)
{
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    memset(c, 0, sizeof *c);
    snprintf(c->id, sizeof c->id, "%04u%02u%02u-%02u%02u%02u", (unsigned)(tm.tm_year + 1900) % 10000u, (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u, (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u);
    char dir[128]; month_dir(dir, sizeof dir, "calls", &tm);
    snprintf(c->path, sizeof c->path, "%.120s/%.24s.jsonl", dir, c->id);
    c->started_ms = (int64_t)now * 1000;
    return ESP_OK;
}

esp_err_t store_call_turn(store_call_t *c, const char *json_line)
{
    c->turns++;
    bool ok = file_append(c->path, json_line, strlen(json_line)) && file_append(c->path, "\n", 1);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t store_call_end(store_call_t *c, const char *summary_json)
{
    char path[200]; snprintf(path, sizeof path, "%s/calls/index.jsonl", s_data);
    char row[512];
    snprintf(row, sizeof row, "{\"id\":\"%s\",\"file\":\"%s\",\"turns\":%d,\"started_ms\":%lld,\"summary\":%s}\n",
             c->id, c->path + strlen(s_data) + 1, c->turns, (long long)c->started_ms, summary_json ? summary_json : "null");
    return file_append(path, row, strlen(row)) ? ESP_OK : ESP_FAIL;
}

void store_event(const char *fmt, ...)
{
    char line[256]; int n = 0;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    n = snprintf(line, sizeof line, "%02d-%02d %02d:%02d:%02d ", (tm.tm_mon + 1) % 100, tm.tm_mday % 100, tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100);
    va_list ap; va_start(ap, fmt); n += vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap); va_end(ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n'; line[n] = '\0';
    char path[200]; snprintf(path, sizeof path, "%s/logs/events.log", s_data);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 64 * 1024) {      // keep the tail: rotate once
        char old[200]; snprintf(old, sizeof old, "%s/logs/events.1.log", s_data);
        remove(old); rename(path, old);
    }
    file_append(path, line, (size_t)n);
}

esp_err_t store_ledger_add(const char *json_line)
{
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char path[200]; snprintf(path, sizeof path, "%s/ledger/%04d-%02d.jsonl", s_data, tm.tm_year + 1900, tm.tm_mon + 1);
    bool ok = file_append(path, json_line, strlen(json_line)) && file_append(path, "\n", 1);
    return ok ? ESP_OK : ESP_FAIL;
}
