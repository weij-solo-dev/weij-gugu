// parent.h — parent mode: a hotspot (or the home LAN) and a small web page served by the device:
// status, settings, Wi-Fi and vendor keys (write-only), the call log, export. v1.0 scope: read logs
// and export; the captive portal and the device-screen password page follow in stage 1.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t   parent_init(void);              // starts the web server on the station interface (host: localhost:8088)
esp_err_t   parent_enter(void);             // bring up the hotspot gugu-xxxx with a fresh numeric password
void        parent_exit(void);
bool        parent_is_active(void);
const char *parent_hotspot_ssid(void);
const char *parent_hotspot_password(void);
