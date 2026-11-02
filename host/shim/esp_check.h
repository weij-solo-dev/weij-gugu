// host shim — ESP_RETURN_ON_ERROR / ESP_RETURN_ON_FALSE as used by shared code.
#pragma once
#include "esp_err.h"
#include <stdio.h>
#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) { \
    fprintf(stderr, "E %s: " fmt " (%s)\n", tag, ##__VA_ARGS__, esp_err_to_name(err_rc_)); return err_rc_; } } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, fmt, ...) do { if (!(a)) { \
    fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__); return (err); } } while (0)
