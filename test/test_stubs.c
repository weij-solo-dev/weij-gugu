// test_stubs.c — platform pieces the unit-test binary does not link (SDL audio, LVGL ui, the board, the page's server).
#include "audio/audio.h"
#include "board/board.h"
#include "parent/parent.h"
esp_err_t audio_platform_capture(bool on) { (void)on; return ESP_OK; }
const uint32_t audio_out_latency_ms = 0;          // the tests pull audio themselves: heard as soon as pulled
int  board_battery_mv(void) { return -1; }
bool board_is_charging(void) { return false; }
bool parent_is_active(void) { return false; }
