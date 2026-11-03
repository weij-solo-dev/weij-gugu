// board_host.c — the StopWatch as an SDL window: a 466×466 RGB565 framebuffer, the mouse as the
// touch panel, B / Y (or left / right arrow) as the blue / yellow keys. The SDL calls that must run
// on the main thread are invoked from host/main.c; this file keeps the state.
#define LOG_TAG "board"
#include "board/board.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include <SDL.h>
#include <string.h>
#include <stdatomic.h>

static uint16_t s_fb[BOARD_DISPLAY_W * BOARD_DISPLAY_H];
static atomic_bool s_dirty;
static int s_scale = 1;
static board_listener_t s_listener;
static void *s_listener_user;
static atomic_bool s_touch_down;
static atomic_int s_tx, s_ty;
static bool s_key_down[BOARD_KEY_COUNT], s_key_held[BOARD_KEY_COUNT];
static int64_t s_key_t[BOARD_KEY_COUNT];
static SDL_mutex *s_fb_mutex;

static void emit(board_event_kind_t kind, int a, int b)
{
    board_listener_t cb = s_listener;
    if (!cb) return;
    board_event_t ev = { .kind = kind, .a = a, .b = b, .t_us = clock_us() };
    cb(&ev, s_listener_user);
}

void board_host_begin(int scale)
{
    s_scale = scale;
    s_fb_mutex = SDL_CreateMutex();
}

esp_err_t board_init(void)
{
    LOGI("host board: SDL window %dx%d scale %d (B/Y or arrows = keys, mouse = touch)",
         BOARD_DISPLAY_W, BOARD_DISPLAY_H, s_scale);
    return ESP_OK;
}

void board_set_listener(board_listener_t cb, void *user)
{
    s_listener_user = user;
    s_listener = cb;
}

esp_err_t board_display_flush(int x1, int y1, int x2, int y2, const uint8_t *rgb565,
                              void (*done)(void *user), void *user)
{
    int w = x2 - x1 + 1;
    const uint16_t *src = (const uint16_t *)rgb565;
    SDL_LockMutex(s_fb_mutex);
    for (int y = y1; y <= y2; y++) {
        uint16_t *dst = &s_fb[y * BOARD_DISPLAY_W + x1];
        for (int x = 0; x < w; x++) {
            uint16_t v = *src++;
            dst[x] = (uint16_t)((v << 8) | (v >> 8));   // ui swapped bytes for the panel; undo for SDL
        }
    }
    SDL_UnlockMutex(s_fb_mutex);
    atomic_store(&s_dirty, true);
    if (done) done(user);
    return ESP_OK;
}

void board_display_set_brightness(uint8_t percent) { (void)percent; }
void board_display_power(bool on) { (void)on; }

bool board_touch_read(int *x, int *y)
{
    if (x) *x = atomic_load(&s_tx);
    if (y) *y = atomic_load(&s_ty);
    return atomic_load(&s_touch_down);
}

bool board_key_is_down(board_key_t key) { return key < BOARD_KEY_COUNT && s_key_down[key]; }
int board_battery_mv(void) { return 3900; }
bool board_is_charging(void) { return true; }
void board_vibrate(uint16_t ms) { LOGI("bzzt %u ms", ms); }
void *board_i2c_bus(void) { return NULL; }

// ---- called from the SDL main thread ----
static int key_of(SDL_Keycode k)
{
    if (k == SDLK_b || k == SDLK_LEFT) return BOARD_KEY_BLUE;
    if (k == SDLK_y || k == SDLK_RIGHT) return BOARD_KEY_YELLOW;
    return -1;
}

void board_host_sdl_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
        atomic_store(&s_tx, e->button.x / s_scale); atomic_store(&s_ty, e->button.y / s_scale);
        atomic_store(&s_touch_down, true);
        emit(BOARD_EV_TOUCH_DOWN, e->button.x / s_scale, e->button.y / s_scale);
        break;
    case SDL_MOUSEMOTION:
        if (atomic_load(&s_touch_down)) { atomic_store(&s_tx, e->motion.x / s_scale); atomic_store(&s_ty, e->motion.y / s_scale); }
        break;
    case SDL_MOUSEBUTTONUP:
        atomic_store(&s_touch_down, false);
        emit(BOARD_EV_TOUCH_UP, e->button.x / s_scale, e->button.y / s_scale);
        break;
    case SDL_KEYDOWN: {
        if (e->key.keysym.sym == SDLK_s && !e->key.repeat) emit(BOARD_EV_SHAKE, 160, 0);   // hold S: a shake every repeat
        else if (e->key.keysym.sym == SDLK_s) emit(BOARD_EV_SHAKE, 160, 0);
        if (e->key.keysym.sym == SDLK_p && !e->key.repeat) emit(BOARD_EV_PICKUP, 0, 0);
        int k = key_of(e->key.keysym.sym);
        if (k >= 0 && !e->key.repeat && !s_key_down[k]) { s_key_down[k] = true; s_key_held[k] = false; s_key_t[k] = clock_us(); emit(BOARD_EV_KEY_DOWN, k, 0); }
        break;
    }
    case SDL_KEYUP: {
        int k = key_of(e->key.keysym.sym);
        if (k >= 0 && s_key_down[k]) { s_key_down[k] = false; emit(BOARD_EV_KEY_UP, k, (int)((clock_us() - s_key_t[k]) / 1000)); }
        break;
    }
    default:
        break;
    }
}

void board_host_present(SDL_Renderer *r, SDL_Texture *t)
{
    for (int k = 0; k < BOARD_KEY_COUNT; k++)   // the long press fires while the key is still down, like the device's sampler
        if (s_key_down[k] && !s_key_held[k] && clock_us() - s_key_t[k] >= (int64_t)BOARD_HOLD_MS * 1000) { s_key_held[k] = true; emit(BOARD_EV_KEY_HOLD, k, BOARD_HOLD_MS); }
    if (!atomic_exchange(&s_dirty, false)) return;
    SDL_LockMutex(s_fb_mutex);
    SDL_UpdateTexture(t, NULL, s_fb, BOARD_DISPLAY_W * 2);
    SDL_UnlockMutex(s_fb_mutex);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_RenderCopy(r, t, NULL, NULL);
    SDL_RenderPresent(r);
}

void board_host_screenshot(SDL_Renderer *r, const char *path)
{
    (void)r;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, BOARD_DISPLAY_W, BOARD_DISPLAY_H, 16, SDL_PIXELFORMAT_RGB565);
    SDL_LockMutex(s_fb_mutex);
    memcpy(s->pixels, s_fb, sizeof s_fb);
    SDL_UnlockMutex(s_fb_mutex);
    if (SDL_SaveBMP(s, path) == 0) LOGI("screenshot saved: %s", path);
    else LOGE("screenshot failed: %s", SDL_GetError());
    SDL_FreeSurface(s);
}
