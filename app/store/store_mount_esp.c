// store_mount_esp.c — mount the two LittleFS partitions: `content` (read-only image built from
// content/) at /content and `data` (writable, formatted on first boot) at /data.
#define LOG_TAG "store"
#include "store_internal.h"
#include "foundation/log.h"
#include "esp_littlefs.h"

static esp_err_t mount(const char *label, const char *path, bool read_only, bool format_if_failed)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = path,
        .partition_label = label,
        .format_if_mount_failed = format_if_failed,
        .read_only = read_only,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) { LOGE("mount %s at %s failed: %s", label, path, esp_err_to_name(err)); return err; }
    size_t total = 0, used = 0;
    esp_littlefs_info(label, &total, &used);
    LOGI("%s mounted at %s: %u/%u KB used", label, path, (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

esp_err_t store_mount_all(void)
{
    esp_err_t a = mount("content", "/content", true, false);
    esp_err_t b = mount("data", "/data", false, true);
    return a == ESP_OK && b == ESP_OK ? ESP_OK : ESP_FAIL;
}
