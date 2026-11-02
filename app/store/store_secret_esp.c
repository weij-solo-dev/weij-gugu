// store_secret_esp.c — secrets in NVS (the partition is encrypted once flash encryption is enabled on
// the production build; development boards keep it plain). Namespace "secrets".
#define LOG_TAG "store"
#include "store_internal.h"
#include "foundation/log.h"
#include "nvs_flash.h"
#include "nvs.h"

esp_err_t store_secret_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t store_secret_get(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("secrets", NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t len = cap;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK && len > 1 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t store_secret_set(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("secrets", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
