// foundation/log.h — one logging façade for device and host (CLAUDE.md rule 8: never log secrets).
// Every module defines `#define LOG_TAG "<module>"` before including this header.
#pragma once

#ifndef LOG_TAG
#define LOG_TAG "gugu"
#endif

#ifdef GUGU_HOST
#include <stdio.h>
#include <stdint.h>
int64_t clock_us(void);
void log_host_emit(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));   // foundation/logcap.c: stderr + the capture ring
#define GUGU_LOG(level, fmt, ...) log_host_emit(level, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) GUGU_LOG('E', fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) GUGU_LOG('W', fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) GUGU_LOG('I', fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) do { if (log_debug_enabled()) GUGU_LOG('D', fmt, ##__VA_ARGS__); } while (0)
int log_debug_enabled(void);
#else
#include "esp_log.h"
#define LOGE(fmt, ...) ESP_LOGE(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) ESP_LOGW(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) ESP_LOGI(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) ESP_LOGD(LOG_TAG, fmt, ##__VA_ARGS__)
#endif

// Mask a secret for logging: keeps the first 4 characters, e.g. "sk-c..." — pass the result, never the secret.
const char *log_mask(const char *secret, char out[8]);
