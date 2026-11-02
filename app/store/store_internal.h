#pragma once
#include "store/store.h"
esp_err_t store_secret_init(void);
esp_err_t store_mount_all(void);   // device: LittleFS partitions; host: no-op
