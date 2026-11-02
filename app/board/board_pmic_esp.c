// board_pmic.c — M5PM1 power management IC (I2C 0x6E): charger, boost, battery voltage, its GPIOs.
// v1.0 uses it only to keep the rails the factory firmware keeps and to read the battery; the L1
// sleep / wake-source work belongs to a later milestone (PLAN.md 原型 v1.0 不做：省电与休眠).
#define LOG_TAG "board"
#include "board_internal.h"
#include "foundation/log.h"

#define PM1_REG_DEVICE_ID   0x00
#define PM1_REG_PWR_CFG     0x06   // bit0 CHG_EN, bit1 DCDC_EN, bit2 LDO_EN, bit3 BOOST_EN, bit4 LED level
#define PM1_REG_GPIO_MODE   0x10   // bit n: 1 = output
#define PM1_REG_GPIO_OUT    0x11
#define PM1_REG_GPIO_IN     0x12
#define PM1_REG_VBAT_L      0x22   // mV, 16-bit little-endian (verified 0x103C = 4156 mV)

#define PM1_PWR_CHG_EN      (1u << 0)
#define PM1_PWR_BOOST_EN    (1u << 3)

static i2c_master_dev_handle_t s_dev;

static esp_err_t rd8(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, v, 1, 100);
}

static esp_err_t wr8(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static esp_err_t update8(uint8_t reg, uint8_t mask, bool set)
{
    uint8_t v;
    esp_err_t err = rd8(reg, &v);
    if (err != ESP_OK) return err;
    v = set ? (v | mask) : (v & ~mask);
    return wr8(reg, v);
}

esp_err_t board_pmic_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_PMIC_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c, &cfg, &s_dev);
    if (err != ESP_OK) return err;
    uint8_t id = 0;
    err = rd8(PM1_REG_DEVICE_ID, &id);
    if (err != ESP_OK) {
        LOGE("M5PM1 not responding at 0x%02X", BOARD_PMIC_ADDR);
        return err;
    }
    // Same rails the factory firmware enables. Only ever OR bits in: clearing LDO_EN would cut our own power.
    update8(PM1_REG_PWR_CFG, PM1_PWR_CHG_EN | PM1_PWR_BOOST_EN, true);
    // GPIO2 = charge state (input), GPIO3 = charge current select (output low = 425 mA).
    update8(PM1_REG_GPIO_MODE, 1u << PMIC_GPIO_CHARGE_STATE, false);
    update8(PM1_REG_GPIO_MODE, 1u << PMIC_GPIO_CHARGE_PROG, true);
    update8(PM1_REG_GPIO_OUT, 1u << PMIC_GPIO_CHARGE_PROG, false);
    LOGI("M5PM1 id=0x%02X vbat=%d mV charging=%d", id, board_battery_mv(), (int)board_is_charging());
    return ESP_OK;
}

int board_battery_mv(void)
{
    uint8_t reg = PM1_REG_VBAT_L, b[2];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, 2, 100) != ESP_OK) return -1;
    return (int)(b[0] | (b[1] << 8));
}

bool board_is_charging(void)
{
    uint8_t v;
    if (rd8(PM1_REG_GPIO_IN, &v) != ESP_OK) return false;
    return (v & (1u << PMIC_GPIO_CHARGE_STATE)) == 0;   // active low
}
