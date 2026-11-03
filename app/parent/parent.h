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
const char *parent_hotspot_url(void);       // the page address on the hotspot, e.g. http://192.168.4.1
// Upward hooks app.c wires at init: actions the page triggers in modules parent may not include.
typedef struct {
    void (*trip_delete)(void *user);
    void (*memory_forget)(const char *book, const char *key, void *user);   // book: journey (key = trip id) | about_child | canon_grown (key = index)
    void (*test_sound)(void *user);                                        // the page's "test the speaker" button
    int  (*test_audio)(char *json, size_t cap, void *user);                 // "test speaker and microphone": a beep listened to; the levels as JSON
    void (*apply)(bool wifi_changed, void *user);                          // secrets were saved: take them into use now (keys at once, Wi-Fi joins)
    void *user;
} parent_hooks_t;
void parent_set_hooks(const parent_hooks_t *h);
