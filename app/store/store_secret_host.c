// store_secret_host.c — secrets from the environment (GUGU_<KEY>); nothing is written to disk on the host.
#define LOG_TAG "store"
#include "store_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static void env_name(const char *key, char *out, size_t cap)
{
    size_t o = (size_t)snprintf(out, cap, "GUGU_");
    for (const char *p = key; *p && o + 1 < cap; p++) out[o++] = (char)(isalnum((unsigned char)*p) ? toupper((unsigned char)*p) : '_');
    out[o] = '\0';
}

esp_err_t store_secret_init(void) { return ESP_OK; }

esp_err_t store_secret_get(const char *key, char *out, size_t cap)
{
    char name[64]; env_name(key, name, sizeof name);
    const char *v = getenv(name);
    if (!v || !*v) return ESP_ERR_NOT_FOUND;
    snprintf(out, cap, "%s", v);
    return ESP_OK;
}

esp_err_t store_secret_set(const char *key, const char *value)
{
    char name[64]; env_name(key, name, sizeof name);
    return setenv(name, value ? value : "", 1) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t store_mount_all(void) { return ESP_OK; }
