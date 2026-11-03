// store_export.c — the captured log on flash (logs/console.log, rotated) and the one-file log bundle the
// owner downloads from the pairing page to send for diagnosis. Everything a remote look needs: the boot
// record, settings, which secrets are set (never their values), the event log, the console log, the last
// calls, the current trip and the journey's tail. Secret values are masked across the whole bundle before it
// leaves (CLAUDE.md rule 8).
#define LOG_TAG "store"
#include "store/store.h"
#include "store_internal.h"
#include "foundation/log.h"
#include "foundation/logcap.h"
#include "foundation/mem.h"
#include "foundation/strbuf.h"
#include "foundation/task.h"
#include "foundation/atomic_file.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#define CONSOLE_ROTATE_BYTES (128 * 1024)
static mutex_t s_flush_lock;

static long file_size(const char *path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : -1; }

void store_logs_flush(void)
{
    if (!logcap_active()) return;
    if (!s_flush_lock) s_flush_lock = mutex_create();
    mutex_lock(s_flush_lock);
    static char *buf;
    if (!buf) buf = mem_alloc_big(16384);
    if (buf) {
        char path[200], old[200];
        snprintf(path, sizeof path, "%s/logs/console.log", store_data_dir());
        snprintf(old, sizeof old, "%s/logs/console.1.log", store_data_dir());
        size_t n;
        while ((n = logcap_take(buf, 16384)) > 0) {
            if (file_size(path) + (long)n > CONSOLE_ROTATE_BYTES) { remove(old); rename(path, old); }
            file_append(path, buf, n);
        }
    }
    mutex_unlock(s_flush_lock);
}

// ---- masking ----
static char *find(char *hay, size_t n, const char *needle, size_t k)   // memmem is not in newlib's default profile
{
    for (size_t i = 0; k && i + k <= n; i++) if (hay[i] == needle[0] && !memcmp(hay + i, needle, k)) return hay + i;
    return NULL;
}

static size_t mask_value(char *s, size_t n, const char *secret)
{
    size_t k = strlen(secret);
    if (k < 6) return n;                        // too short to search for safely; such values are never logged anyway
    for (char *p = s; (p = find(p, n - (size_t)(p - s), secret, k)) != NULL;) {
        memcpy(p, "***", 3);
        memmove(p + 3, p + k, n - (size_t)(p - s) - k);
        n -= k - 3;
        p += 3;
    }
    s[n] = '\0';
    return n;
}

static bool key_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

size_t store_mask_secrets(char *s, size_t n)
{
    static const char *const SECRETS[] = { "wifi_pass", "minimax_api_key", "qwen_api_key", "volc_api_key", "volc_ark_api_key", NULL };
    for (int i = 0; SECRETS[i]; i++) {
        char v[256];
        if (store_secret_get(SECRETS[i], v, sizeof v) == ESP_OK && v[0]) n = mask_value(s, n, v);
    }
    // anything shaped like a vendor key ("sk-" and a long run of key characters) is masked as well
    for (size_t i = 0; i + 3 < n; i++) {
        if (s[i] != 's' || s[i + 1] != 'k' || s[i + 2] != '-') continue;
        size_t j = i + 3; while (j < n && key_char(s[j])) j++;
        if (j - i < 11) continue;
        memcpy(s + i + 3, "***", 3);
        memmove(s + i + 6, s + j, n - j);
        n -= (j - i) - 6;
    }
    s[n] = '\0';
    return n;
}

// ---- the bundle ----
static void section(strbuf_t *b, const char *title) { strbuf_printf(b, "\n==== %s ====\n", title); }

static void put_file(strbuf_t *b, const char *rel, size_t max_tail)   // the whole file, or its last max_tail bytes from a line start
{
    char path[200]; snprintf(path, sizeof path, "%s/%s", store_data_dir(), rel);
    size_t n; char *txt = file_read_all(path, &n);
    if (!txt) { strbuf_printf(b, "(no %s)\n", rel); return; }
    const char *start = txt;
    if (max_tail && n > max_tail) { start = txt + n - max_tail; const char *nl = memchr(start, '\n', n - max_tail); if (nl) start = nl + 1; strbuf_puts(b, "[...]\n"); }
    strbuf_append(b, start, n - (size_t)(start - txt));
    if (n && txt[n - 1] != '\n') strbuf_putc(b, '\n');
    mem_release(txt);
}

static void put_tail_lines(strbuf_t *b, const char *rel, int lines)
{
    char path[200]; snprintf(path, sizeof path, "%s/%s", store_data_dir(), rel);
    size_t n; char *txt = file_read_all(path, &n);
    if (!txt) { strbuf_printf(b, "(no %s)\n", rel); return; }
    const char *p = txt + n; int seen = 0;
    while (p > txt && seen <= lines) { p--; if (*p == '\n' && p != txt + n - 1) seen++; }
    if (*p == '\n') p++;
    strbuf_append(b, p, n - (size_t)(p - txt));
    if (n && txt[n - 1] != '\n') strbuf_putc(b, '\n');
    mem_release(txt);
}

char *store_export_logs(size_t *len)
{
    store_logs_flush();                         // the newest lines are still in the ring
    strbuf_t b; strbuf_init(&b);
    section(&b, "boot record (logs/boot.json)"); put_file(&b, "logs/boot.json", 0);
    section(&b, "settings.json"); put_file(&b, "settings.json", 0);
    section(&b, "secrets (set or not; values never leave the device)");
    {
        static const char *const KEYS[] = { "wifi_ssid", "wifi_pass", "minimax_api_key", "qwen_api_key", "volc_api_key", "volc_ark_api_key", NULL };
        for (int i = 0; KEYS[i]; i++) {
            char v[256]; bool set = store_secret_get(KEYS[i], v, sizeof v) == ESP_OK && v[0];
            if (!set) strbuf_printf(&b, "%s: not set\n", KEYS[i]);
            else if (!strcmp(KEYS[i], "wifi_ssid")) strbuf_printf(&b, "%s: \"%s\"\n", KEYS[i], v);
            else { char m[8]; strbuf_printf(&b, "%s: set (%s, %u chars)\n", KEYS[i], strcmp(KEYS[i], "wifi_pass") ? log_mask(v, m) : "***", (unsigned)strlen(v)); }
        }
    }
    section(&b, "event log (logs/events.1.log, logs/events.log)"); put_file(&b, "logs/events.1.log", 0); put_file(&b, "logs/events.log", 0);
    section(&b, "console log (logs/console.1.log, logs/console.log)"); put_file(&b, "logs/console.1.log", 0); put_file(&b, "logs/console.log", 0);
    section(&b, "calls (calls/index.jsonl, last 20)"); put_tail_lines(&b, "calls/index.jsonl", 20);
    {   // the turns of the last three calls
        char path[200]; snprintf(path, sizeof path, "%s/calls/index.jsonl", store_data_dir());
        size_t n; char *idx = file_read_all(path, &n);
        const char *files[3] = { 0 }; int nf = 0;
        cJSON *rows[64]; int nr = 0;
        if (idx) for (char *l = strtok(idx, "\n"); l && nr < 64; l = strtok(NULL, "\n")) { cJSON *r = cJSON_Parse(l); if (r) rows[nr++] = r; else continue; }
        for (int i = nr - 1; i >= 0 && nf < 3; i--) { const char *f = cJSON_GetStringValue(cJSON_GetObjectItem(rows[i], "file")); if (f) files[nf++] = f; }
        for (int i = nf - 1; i >= 0; i--) { char t[200]; snprintf(t, sizeof t, "call %s", files[i]); section(&b, t); put_file(&b, files[i], 8192); }
        for (int i = 0; i < nr; i++) cJSON_Delete(rows[i]);
        mem_release(idx);
    }
    section(&b, "trip (trips/current.json)"); put_file(&b, "trips/current.json", 0);
    section(&b, "journey (memory/journey.jsonl, last 15)"); put_tail_lines(&b, "memory/journey.jsonl", 15);
    {
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        char rel[64]; snprintf(rel, sizeof rel, "ledger/%04d-%02d.jsonl", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100);
        section(&b, "cost ledger (this month, last 20)"); put_tail_lines(&b, rel, 20);
    }
    if (!b.s) { *len = 0; return NULL; }
    b.len = store_mask_secrets(b.s, b.len);
    *len = b.len;
    return strbuf_detach(&b);
}
