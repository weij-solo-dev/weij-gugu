// board_touch.c — CST820B capacitive touch (I2C 0x15). Polled from the input timer; LVGL reads the cache.
#define LOG_TAG "board"
#include "board_internal.h"
#include "foundation/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static i2c_master_dev_handle_t s_dev;
static volatile bool s_down;
static volatile int s_x, s_y;

esp_err_t board_touch_init(void)
{
    board_ioe_write(IOE_P_TOUCH_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    board_ioe_write(IOE_P_TOUCH_RST, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TOUCH_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c, &cfg, &s_dev);
    if (err != ESP_OK) return err;
    uint8_t reg = 0xA7, chip = 0;   // chip id register on the CST8xx family
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &chip, 1, 100) == ESP_OK) LOGI("touch chip id 0x%02X", chip);
    else LOGW("touch controller did not answer (will keep polling)");
    return ESP_OK;
}

// Called every 20 ms from the input timer. Emits down/up transitions.
void board_touch_poll(void)
{
    uint8_t reg = 0x01, d[6];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, d, 6, 50) != ESP_OK) return;
    bool down = (d[1] & 0x0F) > 0;
    int x = ((d[2] & 0x0F) << 8) | d[3];
    int y = ((d[4] & 0x0F) << 8) | d[5];
    if (down) { s_x = x; s_y = y; }
    if (down != s_down) {
        s_down = down;
        board_event_t ev = { .kind = down ? BOARD_EV_TOUCH_DOWN : BOARD_EV_TOUCH_UP, .a = s_x, .b = s_y,
                             .t_us = esp_timer_get_time() };
        board_emit(&ev);
    }
}

bool board_touch_read(int *x, int *y)
{
    if (x) *x = s_x;
    if (y) *y = s_y;
    return s_down;
}
