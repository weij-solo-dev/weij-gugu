// board.c — façade: bring-up order, event fan-out, vibration, shared handles.
#define LOG_TAG "board"
#include "board_internal.h"
#include "esp_system.h"
#include "esp_core_dump.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <stdlib.h>
#include "foundation/log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

i2c_master_bus_handle_t board_i2c;
static board_listener_t s_listener;
static void *s_listener_user;
static esp_timer_handle_t s_motor_timer;

void board_emit(const board_event_t *ev)
{
    board_listener_t cb = s_listener;
    if (cb) cb(ev, s_listener_user);
}

void board_set_listener(board_listener_t cb, void *user)
{
    s_listener_user = user;
    s_listener = cb;
}

void *board_i2c_bus(void)
{
    return board_i2c;
}

static void motor_off(void *arg)
{
    board_ioe_write(IOE_P_MOTOR, false);
}

const char *board_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_EXT:        return "external reset pin";
    case ESP_RST_SW:         return "software restart";
    case ESP_RST_PANIC:      return "crash (panic)";
    case ESP_RST_INT_WDT:    return "interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "task watchdog";
    case ESP_RST_WDT:        return "watchdog";
    case ESP_RST_DEEPSLEEP:  return "wake from deep sleep";
    case ESP_RST_BROWNOUT:   return "brownout (power dip)";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb (serial port opened)";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu lockup";
    default:                 return "unknown";
    }
}

bool board_crash_report(char *what, size_t what_cap, char *bt, size_t bt_cap)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (!what || !what_cap || !bt || !bt_cap || esp_core_dump_image_check() != ESP_OK) return false;
    esp_core_dump_summary_t *d = calloc(1, sizeof *d);       // ~250 B: off the caller's stack
    if (!d) return false;
    bool ok = esp_core_dump_get_summary(d) == ESP_OK;
    if (ok) {
        char reason[80] = "";
        if (esp_core_dump_get_panic_reason(reason, sizeof reason) != ESP_OK) reason[0] = '\0';
        snprintf(what, what_cap, "task %.16s, %s, pc 0x%08lx, cause %lu, vaddr 0x%08lx, elf %.8s", d->exc_task, reason[0] ? reason : "exception",
                 (unsigned long)d->exc_pc, (unsigned long)d->ex_info.exc_cause, (unsigned long)d->ex_info.exc_vaddr, (const char *)d->app_elf_sha256);
        size_t n = 0; bt[0] = '\0';
        for (uint32_t i = 0; i < d->exc_bt_info.depth && i < 16 && n + 12 < bt_cap; i++)
            n += (size_t)snprintf(bt + n, bt_cap - n, "%s0x%08lx", i ? " " : "", (unsigned long)d->exc_bt_info.bt[i]);
        if (d->exc_bt_info.corrupted && n + 12 < bt_cap) snprintf(bt + n, bt_cap - n, " (corrupted)");
    }
    free(d);
    esp_core_dump_image_erase();
    return ok;
#else
    (void)what; (void)what_cap; (void)bt; (void)bt_cap;
    return false;
#endif
}

void board_firmware_id(char *out, size_t cap)
{
    char sha[17] = "";
    esp_app_get_elf_sha256(sha, sizeof sha);
    snprintf(out, cap, "%.8s", sha);
}

void board_firmware_version(char *out, size_t cap) { snprintf(out, cap, "%s", esp_app_get_description()->version); }   // IDF stamps it at build

void board_vibrate(uint16_t ms)
{
    if (!s_motor_timer) {
        const esp_timer_create_args_t args = { .callback = motor_off, .name = "gugu_board_motor" };
        if (esp_timer_create(&args, &s_motor_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_motor_timer);
    board_ioe_write(IOE_P_MOTOR, true);
    esp_timer_start_once(s_motor_timer, (uint64_t)ms * 1000);
}

esp_err_t board_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &board_i2c), LOG_TAG, "i2c");
    ESP_RETURN_ON_ERROR(board_ioe_init(), LOG_TAG, "ioe");
    ESP_RETURN_ON_ERROR(board_pmic_init(), LOG_TAG, "pmic");

    // Power rails and resets, in the factory order.
    board_ioe_output(IOE_P_LCD_POWER, true);
    board_ioe_output(IOE_P_LCD_RST, true);
    board_ioe_output(IOE_P_TOUCH_RST, true);
    board_ioe_output(IOE_P_CODEC_POWER, true);
    board_ioe_output(IOE_P_PA_EN, true);
    board_ioe_output(IOE_P_MOTOR, false);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(board_display_init(), LOG_TAG, "display");
    ESP_RETURN_ON_ERROR(board_touch_init(), LOG_TAG, "touch");
    ESP_RETURN_ON_ERROR(board_input_init(), LOG_TAG, "input");
    board_imu_init();                                  // optional: a missing IMU only logs
    LOGI("board ready");
    return ESP_OK;
}
