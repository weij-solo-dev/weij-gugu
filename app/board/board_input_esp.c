// board_input.c — the two side keys (debounced) and the touch poll, on one 10 ms timer.
#define LOG_TAG "board"
#include "board_internal.h"
#include "foundation/log.h"
#include "esp_timer.h"

void board_touch_poll(void);

static const gpio_num_t s_key_gpio[BOARD_KEY_COUNT] = { BOARD_KEY_BLUE_GPIO, BOARD_KEY_YELLOW_GPIO };
static uint8_t s_hist[BOARD_KEY_COUNT];      // last 8 samples, 1 = pressed
static bool s_down[BOARD_KEY_COUNT], s_held[BOARD_KEY_COUNT];
static int64_t s_down_t[BOARD_KEY_COUNT];
static esp_timer_handle_t s_timer;
static unsigned s_tick;

static void tick(void *arg)
{
    int64_t now = esp_timer_get_time();
    for (int k = 0; k < BOARD_KEY_COUNT; k++) {
        bool pressed = gpio_get_level(s_key_gpio[k]) == 0;   // active low with pull-up
        s_hist[k] = (uint8_t)((s_hist[k] << 1) | (pressed ? 1 : 0));
        uint8_t last3 = s_hist[k] & 0x07;
        if (!s_down[k] && last3 == 0x07) {
            s_down[k] = true; s_held[k] = false; s_down_t[k] = now;
            board_event_t ev = { .kind = BOARD_EV_KEY_DOWN, .a = k, .b = 0, .t_us = now };
            board_emit(&ev);
        } else if (s_down[k] && !s_held[k] && now - s_down_t[k] >= (int64_t)BOARD_HOLD_MS * 1000) {
            s_held[k] = true;
            board_event_t ev = { .kind = BOARD_EV_KEY_HOLD, .a = k, .b = BOARD_HOLD_MS, .t_us = now };
            board_emit(&ev);
        } else if (s_down[k] && last3 == 0x00) {
            s_down[k] = false;
            board_event_t ev = { .kind = BOARD_EV_KEY_UP, .a = k, .b = (int)((now - s_down_t[k]) / 1000), .t_us = now };
            board_emit(&ev);
        }
    }
    if ((++s_tick & 1) == 0) board_touch_poll();
}

esp_err_t board_input_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_KEY_BLUE_GPIO) | (1ULL << BOARD_KEY_YELLOW_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), LOG_TAG, "key gpio");
    const esp_timer_create_args_t args = { .callback = tick, .name = "gugu_board_input" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_timer), LOG_TAG, "timer");
    return esp_timer_start_periodic(s_timer, 10 * 1000);
}

bool board_key_is_down(board_key_t key)
{
    return key < BOARD_KEY_COUNT && s_down[key];
}
