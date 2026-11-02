// test_stubs.c — platform pieces the unit-test binary does not link (SDL audio, LVGL ui).
#include "audio/audio.h"
esp_err_t audio_platform_capture(bool on) { (void)on; return ESP_OK; }
