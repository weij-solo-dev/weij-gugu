// test_stubs.c — platform pieces the unit-test binary does not link (SDL audio, LVGL ui, the board, the page's server).
#include "audio/audio.h"
#include "board/board.h"
#include "parent/parent_internal.h"
esp_err_t audio_platform_capture(bool on) { (void)on; return ESP_OK; }
const uint32_t audio_out_latency_ms = 0;          // the tests pull audio themselves: heard as soon as pulled
int  board_battery_mv(void) { return -1; }
bool board_is_charging(void) { return false; }
static bool s_hotspot;                            // the page's hotspot, as test_parent drives it
esp_err_t parent_enter(void) { s_hotspot = true; pair_begin(); return ESP_OK; }
void parent_exit(void) { if (s_hotspot) { s_hotspot = false; pair_end(); } }
bool parent_is_active(void) { return s_hotspot; }
const char *parent_hotspot_ssid(void) { return "gugu-7A3F"; }
const char *parent_hotspot_password(void) { return s_hotspot ? "40917263" : ""; }
const char *parent_hotspot_url(void) { return "http://192.168.4.1"; }
