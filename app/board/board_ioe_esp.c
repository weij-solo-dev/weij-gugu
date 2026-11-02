// board_ioe.c — M5IOE1 I/O expander (I2C 0x4F): power rails, LCD/touch reset, PA enable, motor.
// Register map from the vendor library, re-implemented in C (reference only). 16-bit registers are
// little-endian pairs (L at reg, H at reg+1); bit (P-1) of the pair is pin P.
#define LOG_TAG "board"
#include "board_internal.h"
#include "foundation/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define IOE_REG_UID_L       0x00
#define IOE_REG_GPIO_MODE_L 0x03   // 1 = output
#define IOE_REG_GPIO_OUT_L  0x05
#define IOE_REG_GPIO_PU_L   0x09
#define IOE_REG_GPIO_PD_L   0x0B
#define IOE_REG_GPIO_DRV_L  0x13   // 1 = open-drain

static i2c_master_dev_handle_t s_dev;

static esp_err_t rd16(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, 2, 100);
    if (err == ESP_OK) *v = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return err;
}

static esp_err_t wr16(uint8_t reg, uint16_t v)
{
    uint8_t b[3] = { reg, (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    return i2c_master_transmit(s_dev, b, 3, 100);
}

static esp_err_t update16(uint8_t reg, uint16_t mask, bool set)
{
    uint16_t v;
    esp_err_t err = rd16(reg, &v);
    if (err != ESP_OK) return err;
    v = set ? (v | mask) : (v & ~mask);
    return wr16(reg, v);
}

esp_err_t board_ioe_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_IOE_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c, &cfg, &s_dev);
    if (err != ESP_OK) return err;
    uint16_t uid;
    err = rd16(IOE_REG_UID_L, &uid);
    if (err != ESP_OK) {
        LOGE("M5IOE1 not responding at 0x%02X", BOARD_IOE_ADDR);
        return err;
    }
    LOGI("M5IOE1 uid=0x%04X", uid);
    return ESP_OK;
}

esp_err_t board_ioe_output(int p, bool high)
{
    uint16_t m = 1u << (p - 1);
    esp_err_t err = update16(IOE_REG_GPIO_MODE_L, m, true);
    if (err == ESP_OK) err = update16(IOE_REG_GPIO_PU_L, m, false);
    if (err == ESP_OK) err = update16(IOE_REG_GPIO_PD_L, m, false);
    if (err == ESP_OK) err = update16(IOE_REG_GPIO_DRV_L, m, false);   // push-pull
    if (err == ESP_OK) err = update16(IOE_REG_GPIO_OUT_L, m, high);
    return err;
}

esp_err_t board_ioe_write(int p, bool high)
{
    return update16(IOE_REG_GPIO_OUT_L, 1u << (p - 1), high);
}
