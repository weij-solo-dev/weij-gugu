// console_esp.c — a line reader on the USB-Serial/JTAG console (no esp_console REPL: it would take the
// port over from the logger). Lines end with \n; prompt is "gugu> ".
#define LOG_TAG "console"
#include "console/console.h"
#include "foundation/log.h"
#include "foundation/task.h"
#include "foundation/clock.h"
#include "driver/usb_serial_jtag.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int console_dispatch(char *line);

static void console_task(void *arg)
{
    (void)arg;
    char line[256]; size_t n = 0;
    uint8_t buf[64];
    for (;;) {
        int got = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(100));
        for (int i = 0; i < got; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (n) { line[n] = '\0'; printf("\n"); console_dispatch(line); printf("gugu> "); fflush(stdout); fsync(fileno(stdout)); n = 0; }   // fsync pushes the USB TX FIFO out without waiting for a newline
            } else if (c == 8 || c == 127) { if (n) n--; }
            else if (n < sizeof line - 1) line[n++] = c;
        }
    }
}

size_t console_read_raw(uint8_t *buf, size_t n, uint32_t timeout_ms)   // put: bytes as they come, no line editing
{
    size_t got = 0;
    int64_t until = clock_us() + (int64_t)timeout_ms * 1000;
    while (got < n && clock_us() < until) {
        int r = usb_serial_jtag_read_bytes(buf + got, n - got, pdMS_TO_TICKS(50));
        if (r > 0) got += (size_t)r;
    }
    return got;
}

esp_err_t console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 4096;                    // holds a whole 2 KB piece of a put while the console task is busy elsewhere
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!task_spawn("gugu_console", console_task, NULL, 6144, 2, 0)) return ESP_FAIL;
    LOGI("console ready (type help)");
    return ESP_OK;
}
