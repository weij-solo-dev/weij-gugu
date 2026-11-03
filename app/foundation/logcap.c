// foundation/logcap.c — the ring: a byte buffer and two running counters (written, taken). Writers and the
// taker share one mutex; the device hook never blocks for long and never runs from an interrupt.
#include "foundation/logcap.h"
#include "foundation/mem.h"
#include "foundation/task.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static char *s_ring;
static size_t s_size;
static uint64_t s_written, s_taken;
static mutex_t s_lock;

bool logcap_active(void) { return s_ring != NULL; }

static void put(const char *s, size_t n)       // lock held
{
    if (n > s_size) { s += n - s_size; s_written += n - s_size; n = s_size; }
    size_t at = (size_t)(s_written % s_size), first = s_size - at < n ? s_size - at : n;
    memcpy(s_ring + at, s, first);
    if (n > first) memcpy(s_ring, s + first, n - first);
    s_written += n;
}

void logcap_write(const char *s, size_t n)
{
    if (!s_ring || !s || !n) return;
    mutex_lock(s_lock);
    put(s, n);
    mutex_unlock(s_lock);
}

size_t logcap_pending(void)
{
    if (!s_ring) return 0;
    mutex_lock(s_lock);
    uint64_t p = s_written - s_taken;
    mutex_unlock(s_lock);
    return (size_t)(p > s_size ? s_size : p);
}

size_t logcap_take(char *dst, size_t cap)
{
    if (!s_ring || !cap) return 0;
    size_t o = 0;
    mutex_lock(s_lock);
    if (s_written - s_taken > s_size) {          // the ring lapped the taker: say so, then resume at the oldest byte still there
        uint64_t lost = s_written - s_taken - s_size;
        int w = snprintf(dst, cap, "[... %llu bytes of log lost ...]\n", (unsigned long long)lost);
        if (w > 0) o = (size_t)w < cap ? (size_t)w : cap;
        s_taken = s_written - s_size;
    }
    size_t n = (size_t)(s_written - s_taken);
    if (n > cap - o) n = cap - o;
    size_t at = (size_t)(s_taken % s_size), first = s_size - at < n ? s_size - at : n;
    memcpy(dst + o, s_ring + at, first);
    if (n > first) memcpy(dst + o + first, s_ring, n - first);
    s_taken += n;
    mutex_unlock(s_lock);
    return o + n;
}

// "HH:MM:SS " of local time, or nothing before the clock is set (the year is still 1970).
static size_t stamp(char *out, size_t cap)
{
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2024) return 0;
    int w = snprintf(out, cap, "%02d:%02d:%02d ", tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100);
    return w > 0 ? (size_t)w : 0;
}

// Copy a formatted line, dropping ANSI colour sequences, with the time in front. Lock held: the buffer is static
// because the logging task may be a system task with a small stack (the event loop's is 2.3 KB).
static char s_cap[360];
static void capture_line(const char *line, size_t n)
{
    char *buf = s_cap; size_t o = stamp(buf, sizeof s_cap);
    for (size_t i = 0; i < n && o < sizeof s_cap - 1; i++) {
        if (line[i] == '\033' && i + 1 < n && line[i + 1] == '[') { while (i < n && line[i] != 'm') i++; continue; }
        buf[o++] = line[i];
    }
    logcap_write(buf, o);
}

#ifdef GUGU_HOST
#include <stdlib.h>
int64_t clock_us(void);
void log_host_emit(char level, const char *tag, const char *fmt, ...)
{
    char line[360];
    int n = snprintf(line, sizeof line, "%c (%lld) %s: ", level, (long long)(clock_us() / 1000), tag);
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n + (m > 0 ? (size_t)m : 0);
    if (len > sizeof line - 2) len = sizeof line - 2;
    line[len++] = '\n'; line[len] = '\0';
    fputs(line, stderr);
    if (s_ring) { mutex_lock(s_lock); capture_line(line, len); mutex_unlock(s_lock); }
}

bool logcap_init(size_t ring_bytes)
{
    if (s_ring) return true;
    s_lock = mutex_create();
    s_ring = mem_alloc_big(ring_bytes);
    if (!s_ring) return false;
    s_size = ring_bytes;
    return true;
}
#else
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static vprintf_like_t s_prev;
static char s_line[360];

static int capture_vprintf(const char *fmt, va_list ap)
{
    va_list copy; va_copy(copy, ap);
    int r = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);   // the console sees exactly what it saw before
    if (s_ring && !xPortInIsrContext()) {
        mutex_lock(s_lock);                     // recursive: the ring writer below takes it again
        int n = vsnprintf(s_line, sizeof s_line, fmt, copy);
        if (n > 0) capture_line(s_line, (size_t)n < sizeof s_line ? (size_t)n : sizeof s_line - 1);
        mutex_unlock(s_lock);
    }
    va_end(copy);
    return r;
}

bool logcap_init(size_t ring_bytes)
{
    if (s_ring) return true;
    s_lock = mutex_create();
    char *ring = mem_alloc_big(ring_bytes);
    if (!ring) return false;
    s_size = ring_bytes;
    s_ring = ring;
    s_prev = esp_log_set_vprintf(capture_vprintf);
    return true;
}
#endif
