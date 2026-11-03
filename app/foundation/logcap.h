// foundation/logcap.h — a copy of every log line in a PSRAM ring, so the device can hand its own log to the
// owner (the pairing page's "export logs", the console's `logs`) instead of needing a USB cable and a monitor.
// Device: hooks esp_log's vprintf (the console output is unchanged); host: the LOG macros write here too.
// Lines get the local wall-clock time in front once the clock is set. Secrets never reach a log line
// (CLAUDE.md rule 8); the exporter masks again anyway.
#pragma once
#include <stdbool.h>
#include <stddef.h>

bool   logcap_init(size_t ring_bytes);          // allocate the ring and start capturing; false without memory
bool   logcap_active(void);
void   logcap_write(const char *s, size_t n);   // append raw text (the hooks call this; tests may too)
// Bytes logged since the previous take, oldest first, at most `cap`; call again until it returns 0. When the ring
// overran since the last take, the first chunk starts with a "[… N bytes of log lost …]" note.
size_t logcap_take(char *dst, size_t cap);
size_t logcap_pending(void);                    // bytes a take would return (lost bytes excluded)
