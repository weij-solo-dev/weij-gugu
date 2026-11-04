// store.h — files on flash (CLAUDE.md rule 13): settings, the call log, the cost ledger, exports.
// Writable data lives under one directory (device: /data, host: host/data); content is read-only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t   store_init(const char *data_dir, const char *content_dir);
const char *store_data_dir(void);
const char *store_content_dir(void);

// ---- settings.json (flat string/int map; defaults when missing) ----
const char *store_setting_str(const char *key, const char *dflt);
int         store_setting_int(const char *key, int dflt);
esp_err_t   store_setting_set_str(const char *key, const char *value);   // persists atomically
esp_err_t   store_setting_set_int(const char *key, int value);

// ---- secrets (device: encrypted NVS; host: environment / secrets file) ----
esp_err_t   store_secret_get(const char *key, char *out, size_t cap);    // ESP_ERR_NOT_FOUND if unset
esp_err_t   store_secret_set(const char *key, const char *value);
// The secrets that may exist: Wi-Fi built in, vendor keys declared by the provider presets. The export lists them as
// set or not and masks their values, the page shows the owner's own; no module keeps a vendor list of its own.
void        store_secret_declare(const char *key);
const char *store_secret_name(int i);                // in declaration order, wifi_ssid and wifi_pass first; NULL past the end

// ---- call log: one JSONL file per call under calls/<yyyy-mm>/, plus calls/index.jsonl ----
typedef struct { char id[32]; char path[160]; int turns; int64_t started_ms; } store_call_t;
esp_err_t   store_call_begin(store_call_t *c);
esp_err_t   store_call_turn(store_call_t *c, const char *json_line);      // appends one turn
esp_err_t   store_call_end(store_call_t *c, const char *summary_json);    // writes the index row

// ---- cost ledger: ledger/<yyyy-mm>.jsonl ----
esp_err_t   store_ledger_add(const char *json_line);

// ---- event log: logs/events.log, one timestamped line per key press, call state, failure; kept under ~64 KB (the parent page shows the tail) ----
void        store_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- the captured log and the export bundle (store_export.c) ----
void        store_logs_flush(void);                   // move what foundation/logcap captured to logs/console.log (rotated at 128 KB)
char       *store_export_logs(size_t *len);           // one text bundle for diagnosis, secrets masked; caller mem_release()s
size_t      store_mask_secrets(char *s, size_t n);    // in place: stored secret values and anything shaped like a key → "***"; returns the new length
