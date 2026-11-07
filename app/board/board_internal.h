// board_internal.h — private to the board module.
#pragma once
#include "board/board.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"

// ---- pin map (M5Stack StopWatch, SKU C152; see PLAN.md hardware section) ----
#define BOARD_I2C_PORT          I2C_NUM_0
#define BOARD_I2C_SDA           GPIO_NUM_47
#define BOARD_I2C_SCL           GPIO_NUM_48
#define BOARD_I2C_HZ            100000

#define BOARD_IOE_ADDR          0x4F   // M5IOE1
#define BOARD_PMIC_ADDR         0x6E   // M5PM1
#define BOARD_TOUCH_ADDR        0x15   // CST820B
#define BOARD_TOUCH_INT         GPIO_NUM_13

#define BOARD_QSPI_HOST         SPI2_HOST
#define BOARD_QSPI_CS           GPIO_NUM_39
#define BOARD_QSPI_SCK          GPIO_NUM_40
#define BOARD_QSPI_D0           GPIO_NUM_41
#define BOARD_QSPI_D1           GPIO_NUM_42
#define BOARD_QSPI_D2           GPIO_NUM_46
#define BOARD_QSPI_D3           GPIO_NUM_45
#define BOARD_QSPI_TE           GPIO_NUM_38

// Official pin map (docs.m5stack.com/en/core/StopWatch, PinMap > KEY): KEYA (YELLOW) = G2, KEYB (BLUE) = G1.
// The factory demo calls them button A/B without colours, which is how the first guess got them swapped.
#define BOARD_KEY_BLUE_GPIO     GPIO_NUM_1   // KEYB, blue: the phone key
#define BOARD_KEY_YELLOW_GPIO   GPIO_NUM_2   // KEYA, yellow: the letter key

#define BOARD_I2S_MCLK          GPIO_NUM_18
#define BOARD_I2S_BCLK          GPIO_NUM_17
#define BOARD_I2S_WS            GPIO_NUM_15
#define BOARD_I2S_DIN           GPIO_NUM_16  // codec ADC -> ESP
#define BOARD_I2S_DOUT          GPIO_NUM_21  // ESP -> codec DAC

// IO expander pins (physical P1..P14).
#define IOE_P_CODEC_POWER       3
#define IOE_P_TOUCH_RST         4
#define IOE_P_LCD_RST           5
#define IOE_P_LCD_POWER         8
#define IOE_P_MOTOR             9
#define IOE_P_PA_EN             10

// PMIC GPIOs.
#define PMIC_GPIO_CHARGE_STATE  2   // input, active low = charging
#define PMIC_GPIO_CHARGE_PROG   3   // output low = 425 mA charge current

// ---- shared state ----
extern i2c_master_bus_handle_t board_i2c;
void board_emit(const board_event_t *ev);

// ioe
esp_err_t board_ioe_init(void);
esp_err_t board_ioe_output(int p, bool high);     // configure P as push-pull output and drive it
esp_err_t board_ioe_write(int p, bool high);
// pmic
esp_err_t board_pmic_init(void);
// display
esp_err_t board_display_init(void);
// touch
esp_err_t board_touch_init(void);
// input
esp_err_t board_input_init(void);
esp_err_t board_imu_init(void);          // BMI270: pick-up events (board_imu_esp.c); never fatal
