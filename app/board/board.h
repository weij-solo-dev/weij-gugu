// board.h — the StopWatch hardware behind one header (CLAUDE.md: board owns display, touch, keys,
// codec, IMU, RTC, PMIC power levels). Everything above this module is platform-independent; the
// simulator provides the same API from board_host.c.
#pragma once
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define BOARD_ISR_FUNC IRAM_ATTR   // for callbacks the board invokes from an interrupt (display flush done)
#else
#define BOARD_ISR_FUNC
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define BOARD_DISPLAY_W 466
#define BOARD_HOLD_MS   3000    // a long press, reported while the finger is still on the key
#define BOARD_DISPLAY_H 466

typedef enum {
    BOARD_KEY_BLUE = 0,     // the "phone" key (push-to-talk)
    BOARD_KEY_YELLOW = 1,   // the "letter" key
    BOARD_KEY_COUNT
} board_key_t;

typedef enum {
    BOARD_EV_KEY_DOWN,      // a = board_key_t
    BOARD_EV_KEY_UP,        // a = board_key_t, b = held ms
    BOARD_EV_KEY_HOLD,      // a = board_key_t, b = ms: fired once while the key is still down, at BOARD_HOLD_MS
    BOARD_EV_TOUCH_DOWN,    // a = x, b = y
    BOARD_EV_TOUCH_UP,      // a = x, b = y
    BOARD_EV_PICKUP,        // IMU: the device left the table (a lasting tilt after calm)
    BOARD_EV_CHARGER,       // a = 1 plugged / 0 unplugged (later)
} board_event_kind_t;

typedef struct {
    board_event_kind_t kind;
    int a, b;
    int64_t t_us;           // esp_timer_get_time() at the event
} board_event_t;

typedef void (*board_listener_t)(const board_event_t *ev, void *user);

// Bring up I2C, the IO expander and PMIC (power rails), the display, touch and keys.
esp_err_t board_init(void);
// Events (keys, touch) are delivered from the board's own task; the receiver copies and returns.
void board_set_listener(board_listener_t cb, void *user);

// ---- display (used by ui) ----
// Push an RGB565 rectangle to the panel; `done` is called (possibly from ISR) when the DMA finished.
esp_err_t board_display_flush(int x1, int y1, int x2, int y2, const uint8_t *rgb565,
                              void (*done)(void *user), void *user);
void board_display_set_brightness(uint8_t percent);
void board_display_power(bool on);

// ---- touch ----
bool board_touch_read(int *x, int *y);   // true while a finger is down

// ---- keys ----
bool board_key_is_down(board_key_t key);

// ---- power ----
const char *board_reset_reason(void);    // why the chip last started: "power-on", "crash (panic)", "brownout (power dip)", …
// A crash on the previous run left a core dump on flash: `what` gets the task, the reason and the faulting address, `bt`
// the backtrace (PCs, decoded on a Mac against the same build's ELF); the dump is then erased so it is told once.
// False when there is none.
bool board_crash_report(char *what, size_t what_cap, char *bt, size_t bt_cap);
void board_firmware_id(char *out, size_t cap);   // the running firmware's ELF SHA-256 (hex, the first 8): which ELF decodes a backtrace
int  board_battery_mv(void);             // -1 on error
bool board_is_charging(void);
void board_vibrate(uint16_t ms);

// ---- codec (used by audio) ----
void *board_i2c_bus(void);               // i2c_master_bus_handle_t, for the codec driver only
