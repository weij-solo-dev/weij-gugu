// board_display.c — 1.75" 466×466 AMOLED (CO5300, QSPI). Init sequence and QSPI parameters follow the
// factory bring-up; pixels are pushed by esp_lcd DMA and completion is reported through a callback so
// the UI never blocks on the panel.
#define LOG_TAG "board"
#include "board_internal.h"
#include "foundation/log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_OPCODE_WRITE_CMD 0x02ULL

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static void (*volatile s_done_cb)(void *);
static void *volatile s_done_user;

static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 0, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 0, 10},   // RGB565
    {0x35, (uint8_t[]){0x00}, 0, 10},   // tearing effect on
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 10},   // brightness
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},   // columns 0..465
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},   // rows 0..465
    {0x11, (uint8_t[]){0x00}, 0, 120},  // sleep out
    {0x29, (uint8_t[]){0x00}, 0, 20},   // display on
};

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *d, void *user)
{
    void (*cb)(void *) = s_done_cb;
    if (cb) cb(s_done_user);
    return false;
}

esp_err_t board_display_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_QSPI_SCK,
        .data0_io_num = BOARD_QSPI_D0,
        .data1_io_num = BOARD_QSPI_D1,
        .data2_io_num = BOARD_QSPI_D2,
        .data3_io_num = BOARD_QSPI_D3,
        .max_transfer_sz = BOARD_DISPLAY_W * BOARD_DISPLAY_H * 2,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_QSPI_HOST, &bus, SPI_DMA_CH_AUTO), LOG_TAG, "spi bus");

    // Hardware reset through the IO expander.
    board_ioe_write(IOE_P_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    board_ioe_write(IOE_P_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = BOARD_QSPI_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = { .quad_mode = 1 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(BOARD_QSPI_HOST, &io, &s_io), LOG_TAG, "panel io");

    co5300_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel, &s_panel), LOG_TAG, "panel");
    esp_lcd_panel_set_gap(s_panel, 0x06, 0);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), LOG_TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), LOG_TAG, "init");
    esp_lcd_panel_invert_color(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), LOG_TAG, "on");
    board_display_set_brightness(80);
    LOGI("display ready (CO5300 QSPI 40 MHz)");
    return ESP_OK;
}

esp_err_t board_display_flush(int x1, int y1, int x2, int y2, const uint8_t *rgb565,
                              void (*done)(void *user), void *user)
{
    s_done_cb = done;
    s_done_user = user;
    return esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, rgb565);
}

// Must not run concurrently with a colour transfer (the panel IO queue is not shared across tasks):
// called during init, and later only from the UI task between frames.
void board_display_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint8_t v = (uint8_t)((255u * percent) / 100u);
    int cmd = (int)((LCD_OPCODE_WRITE_CMD << 24) | (0x51 << 8));
    esp_lcd_panel_io_tx_param(s_io, cmd, &v, 1);
}

void board_display_power(bool on)
{
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, on);
    board_ioe_write(IOE_P_LCD_POWER, on);
}
