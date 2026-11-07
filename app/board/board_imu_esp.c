// board_imu_esp.c — the BMI270 over I2C (espressif/bmi270): a task samples the accelerometer at 50 Hz, the
// foundation's motion detector turns the samples into BOARD_EV_PICKUP (a shake it tells apart only so a rattle is not
// taken for a pick-up; nothing answers one since 2026-11-07, the owner: "摇晃彩蛋请取消"). A missing or silent
// IMU only costs a warning: everything else on the board works without it.
#define LOG_TAG "board"
#include "board_internal.h"
#include "foundation/log.h"
#include "foundation/motion.h"
#include "foundation/clock.h"
#include "foundation/task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bmi270.h"

static bmi270_handle_t *s_imu;

static void imu_task(void *arg)
{
    (void)arg;
    motion_t m; motion_init(&m);
    float x, y, z; int intensity = 0; int errors = 0; uint32_t n = 0;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
        if (bmi270_get_acce_data(s_imu, &x, &y, &z) != ESP_OK) { if (++errors == 10) LOGW("imu: reads failing"); continue; }
        errors = 0;
        if (n++ == 25) LOGI("imu: first second: a = (%.2f, %.2f, %.2f) g", (double)x, (double)y, (double)z);
        motion_event_t ev = motion_feed(&m, x, y, z, clock_ms(), &intensity);
        if (ev == MOTION_PICKUP) { board_event_t e = { .kind = BOARD_EV_PICKUP, .t_us = clock_us() }; board_emit(&e); }
    }
}

esp_err_t board_imu_init(void)
{
    const uint8_t addrs[] = { BMI270_I2C_ADDRESS_L, BMI270_I2C_ADDRESS_H };
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < 2 && !s_imu; i++) {
        bmi270_driver_config_t cfg = { .addr = addrs[i], .interface = BMI270_USE_I2C, .i2c_bus = board_i2c };
        err = bmi270_create(&cfg, &s_imu);
        if (err != ESP_OK) { s_imu = NULL; continue; }
        uint8_t id = 0;
        if (bmi270_get_chip_id(s_imu, &id) != ESP_OK || id != BMI270_CHIP_ID) { bmi270_delete(s_imu); s_imu = NULL; err = ESP_ERR_NOT_FOUND; continue; }
        LOGI("imu: BMI270 at 0x%02x", addrs[i]);
    }
    if (!s_imu) { LOGW("imu: no BMI270 answered (%s); pick-up is off", esp_err_to_name(err)); return ESP_OK; }
    bmi270_config_t mc = { .acce_odr = BMI270_ACC_ODR_50_HZ, .acce_range = BMI270_ACC_RANGE_4_G, .gyro_odr = BMI270_GYR_ODR_25_HZ, .gyro_range = BMI270_GYR_RANGE_2000_DPS };
    err = bmi270_start(s_imu, &mc);
    if (err != ESP_OK) { LOGW("imu: start failed: %s", esp_err_to_name(err)); return ESP_OK; }
    if (!task_spawn("gugu_board_imu", imu_task, NULL, 6144, 2, 0)) LOGW("imu: no task");   // listeners run here
    return ESP_OK;
}
