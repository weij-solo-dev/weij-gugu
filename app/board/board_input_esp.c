// board_input.c — the two side keys (debounced) and the touch poll, every 10 ms on a task of their own. Listeners run
// on it: a key press writes the event log, plays a sound, starts a call or the hotspot. That used to run in an
// esp_timer callback, on the timer task's 3.5 KB stack (PLAN.md 踩坑 35).
#define LOG_TAG "board"
#include "board_internal.h"
#include "foundation/log.h"
#include "foundation/task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void board_touch_poll(void);

static const gpio_num_t s_key_gpio[BOARD_KEY_COUNT] = { BOARD_KEY_BLUE_GPIO, BOARD_KEY_YELLOW_GPIO };
static uint8_t s_hist[BOARD_KEY_COUNT];      // last 8 samples, 1 = pressed
static bool s_down[BOARD_KEY_COUNT], s_held[BOARD_KEY_COUNT];
static int64_t s_down_t[BOARD_KEY_COUNT];
static unsigned s_tick;

static void tick(void)
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

static void input_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) { vTaskDelayUntil(&last, pdMS_TO_TICKS(10)); tick(); }
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
    return task_spawn("gugu_board_input", input_task, NULL, 8192, 5, 0) ? ESP_OK : ESP_FAIL;
}

bool board_key_is_down(board_key_t key)
{
    return key < BOARD_KEY_COUNT && s_down[key];
}
