// parent.h — pairing mode: a hotspot whose page opens by itself on the phone (a captive portal, parent_portal.c) and a small
// web page served by the device: Wi-Fi, status, settings, vendor keys, the call log, export.
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
    void (*volume)(int percent, void *user);                               // the page moved the volume (0–100, already saved): use it now
    int  (*test_audio)(char *json, size_t cap, void *user);                 // "test speaker and microphone": a beep listened to; the levels as JSON
    void (*apply)(void *user);                                             // settings or keys were saved: take them into use now
    void (*wifi_result)(const char *state, void *user);                    // "connect" on the page: trying | ok | bad_password | not_found | failed, on a worker's task
    int  (*check)(char *json, size_t cap, void *user);                     // "test": do the vendors now assigned answer? Waits for the answer
    int  (*voice_test)(const char *lang, const char *voice, char *json, size_t cap, void *user);   // speak the preview line with this voice
    void *user;
} parent_hooks_t;
void parent_set_hooks(const parent_hooks_t *h);
