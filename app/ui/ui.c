// ui.c — LVGL bring-up (display, touch, task), scene composition, pre-scaled pixel art.
// Pixel art is authored at 116×116 (scenes) / 48×54 (the pigeon) and shown 4× with nearest-neighbour.
// The scene and the pigeon's base sprite are scaled once into PSRAM and blitted; only the small overlays
// (beak, eyelids, suitcase) go through LVGL's transform. A repaint therefore costs a memcpy per band
// instead of a per-pixel transform, and the UI task sleeps on a semaphore while the panel DMA runs.
#define LOG_TAG "ui"
#include "ui_internal.h"
#include "board/board.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/task.h"
#include "src/misc/cache/instance/lv_image_cache.h"   // lv_image_cache_drop is not re-exported by lvgl.h
#include <stdatomic.h>
#include <string.h>

// Draw buffers are internal DMA memory: esp_lcd's SPI transport can only DMA from internal RAM, and a
// PSRAM buffer makes spi_master malloc a bounce copy of the whole transfer for every flush (74 KB at
// 80 lines), which fails the moment a call's TLS sessions squeeze internal RAM. Two small internal
// buffers cost 15 KB for good and never allocate; a full-screen repaint is ~60 chunks.
#define UI_BUF_LINES 8

static lv_display_t *s_disp;
static mutex_t s_mutex;
lv_obj_t *ui_gugu_img;                                   // shared with ui_anim.c
static lv_obj_t *s_scene_img, *s_case_img, *s_beak_img, *s_eyes_img;
static ui_emotion_t s_emotion = UI_EMO_NEUTRAL;

// ---- display glue ----
static signal_t s_flush_sig;
static atomic_bool s_flushing;
static atomic_uint s_flushes;   // chunks pushed to the panel; a slow refresh is reported with its chunk count

// Called by the board when the DMA transfer has finished — on the device from the SPI interrupt, so it
// only clears a flag and gives a semaphore; LVGL itself is never touched from interrupt context.
static void BOARD_ISR_FUNC flush_done(void *user)
{
    (void)user;
    atomic_store(&s_flushing, false);
    signal_set_from_isr(s_flush_sig);
}

// LVGL calls this when it needs the buffer that is being flushed: block instead of spinning.
static void flush_wait_cb(lv_display_t *disp)
{
    while (atomic_load(&s_flushing)) signal_wait(s_flush_sig, 10);
    lv_display_flush_ready(disp);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
    atomic_fetch_add(&s_flushes, 1);
    lv_draw_sw_rgb565_swap(px, w * h);   // the panel wants big-endian RGB565
    atomic_store(&s_flushing, true);
    if (board_display_flush(area->x1, area->y1, area->x2, area->y2, px, flush_done, disp) != ESP_OK) {
        atomic_store(&s_flushing, false);
        lv_display_flush_ready(disp);
    }
}

// CO5300 only accepts even start / odd end coordinates.
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1; a->y1 &= ~1;
    a->x2 |= 1;  a->y2 |= 1;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x, y;
    bool down = board_touch_read(&x, &y);
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = x;
    data->point.y = y;
}

static uint32_t tick_cb(void)
{
    return clock_ms();
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        ui_lock();
        int64_t t0 = clock_us();
        unsigned f0 = atomic_load(&s_flushes);
        uint32_t next = lv_timer_handler();
        int64_t took = clock_us() - t0;
        ui_unlock();
        if (took > 150 * 1000) LOGW("slow refresh: %lld ms for %u chunks", (long long)(took / 1000), atomic_load(&s_flushes) - f0);
        if (next > 20) next = 20;
        if (next < 2) next = 2;
        clock_sleep_ms(next);
    }
}

void ui_lock(void)   { mutex_lock(s_mutex); }
void ui_unlock(void) { mutex_unlock(s_mutex); }

// ---- pre-scaled images ----
// A scene (116×116 RGB565 → 464×464, 431 KB) and the pigeon's base sprite (48×54 RGB565A8 → 192×216,
// 124 KB) are expanded once into PSRAM descriptors that LVGL blits without a transform. The pigeon uses
// two slots so that a switch hands LVGL a new pointer while the old one is still the current source.
typedef struct { lv_image_dsc_t dsc; uint8_t *buf; } bigimg_t;
static bigimg_t s_scene_big, s_gugu_big[2];
static int s_gugu_slot;
static const lv_image_dsc_t *s_scene_src, *s_gugu_src;

static bool bigimg_alloc(bigimg_t *b, const lv_image_dsc_t *like)
{
    bool alpha = like->header.cf == LV_COLOR_FORMAT_RGB565A8;
    b->buf = mem_alloc_big((size_t)like->header.w * like->header.h * UI_SPRITE_SCALE * UI_SPRITE_SCALE * (alpha ? 3 : 2));
    return b->buf != NULL;
}

static void bigimg_header(bigimg_t *b, uint32_t W, uint32_t H, bool alpha)
{
    memset(&b->dsc, 0, sizeof b->dsc);
    b->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    b->dsc.header.cf = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    b->dsc.header.w = W;
    b->dsc.header.h = H;
    b->dsc.header.stride = W * 2;
    b->dsc.data_size = (uint32_t)W * H * (alpha ? 3 : 2);
    b->dsc.data = b->buf;
}

// Nearest-neighbour ×UI_SPRITE_SCALE of an RGB565 / RGB565A8 descriptor (colour plane, then alpha plane).
static void upscale(const lv_image_dsc_t *src, bigimg_t *b)
{
    const uint32_t w = src->header.w, h = src->header.h, W = w * UI_SPRITE_SCALE, H = h * UI_SPRITE_SCALE;
    const bool alpha = src->header.cf == LV_COLOR_FORMAT_RGB565A8;
    const uint8_t *sp = src->data, *sa = src->data + (size_t)w * h * 2;
    uint16_t *dp = (uint16_t *)b->buf;
    uint8_t *da = b->buf + (size_t)W * H * 2;
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = dp + (size_t)y * UI_SPRITE_SCALE * W;
        uint8_t *arow = da + (size_t)y * UI_SPRITE_SCALE * W;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = sp + ((size_t)y * w + x) * 2;           // byte-wise: the source lives in flash
            uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
            for (int k = 0; k < UI_SPRITE_SCALE; k++) row[x * UI_SPRITE_SCALE + k] = v;
            if (alpha) memset(arow + x * UI_SPRITE_SCALE, sa[(size_t)y * w + x], UI_SPRITE_SCALE);
        }
        for (int k = 1; k < UI_SPRITE_SCALE; k++) {
            memcpy(row + (size_t)k * W, row, (size_t)W * 2);
            if (alpha) memcpy(arow + (size_t)k * W, arow, W);
        }
    }
    bigimg_header(b, W, H, alpha);
}

void ui_show_backdrop(const art_image_t *img, const art_rgb_t pal[ART_MAX_COLORS])
{
    if (!img || img->w != ART_GRID || img->h != ART_GRID || img->alpha) return;
    ui_lock();
    ui_scene_is_home = false;
    ui_scene_show_window(false);
    lv_image_cache_drop(&s_scene_big.dsc);
    art_render_rgb565(img, pal, UI_SPRITE_SCALE, (uint16_t *)s_scene_big.buf, NULL);
    bigimg_header(&s_scene_big, ART_GRID * UI_SPRITE_SCALE, ART_GRID * UI_SPRITE_SCALE, false);
    s_scene_src = NULL;                 // the next built-in scene must be re-expanded into this buffer
    lv_image_set_src(s_scene_img, &s_scene_big.dsc);
    lv_obj_invalidate(s_scene_img);
    lv_obj_remove_flag(ui_gugu_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_case_img, LV_OBJ_FLAG_HIDDEN);
    ui_unlock();
}

void ui_show_base(const lv_image_dsc_t *base)
{
    if (base == s_gugu_src) return;
    s_gugu_src = base;
    s_gugu_slot ^= 1;
    bigimg_t *b = &s_gugu_big[s_gugu_slot];
    lv_image_cache_drop(&b->dsc);
    upscale(base, b);
    lv_image_set_src(ui_gugu_img, &b->dsc);
}

static void overlay(lv_obj_t *img, const ui_patch_t *p)
{
    if (!p || !p->img) { lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN); return; }
    if (lv_image_get_src(img) != p->img) lv_image_set_src(img, p->img);
    lv_obj_set_pos(img, p->x * UI_SPRITE_SCALE, p->y * UI_SPRITE_SCALE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
}
void ui_show_beak(const ui_patch_t *p) { overlay(s_beak_img, p); }
void ui_show_eyes(const ui_patch_t *p) { overlay(s_eyes_img, p); }

// ---- scene ----
// Small sprites (suitcase, overlays) stay base-resolution descriptors scaled by LVGL.
static lv_obj_t *make_sprite(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y)
{
    lv_obj_t *img = lv_image_create(parent);
    if (src) lv_image_set_src(img, src);
    lv_image_set_pivot(img, 0, 0);
    lv_image_set_scale(img, 256 * UI_SPRITE_SCALE);
    lv_image_set_antialias(img, false);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(img, x, y);
    return img;
}

static void show_scene_src(const lv_image_dsc_t *bg)
{
    if (bg == s_scene_src) return;
    s_scene_src = bg;
    lv_image_cache_drop(&s_scene_big.dsc);
    upscale(bg, &s_scene_big);
    lv_image_set_src(s_scene_img, &s_scene_big.dsc);
    lv_obj_invalidate(s_scene_img);
}

void ui_home_backdrop_refresh(void)
{
    if (ui_scene_is_home) show_scene_src(ui_home_lamp_is_on() ? &scn_home_day : &scn_home_off);
}

void ui_show_scene(ui_scene_t scene, bool night)
{
    ui_lock();
    bool home = scene == UI_SCENE_HOME;
    ui_scene_is_home = home;
    // the room's own night is the window pane (ui_scene) and the lamp; the built-in away scene still has two pictures
    const lv_image_dsc_t *bg = home ? (ui_home_lamp_is_on() ? &scn_home_day : &scn_home_off) : (night ? &scn_away_night : &scn_away_day);
    show_scene_src(bg);
    ui_scene_show_window(home);
    lv_obj_remove_flag(ui_gugu_img, LV_OBJ_FLAG_HIDDEN);   // Gugu is on screen wherever it is; the backdrop changes
    if (home) lv_obj_remove_flag(s_case_img, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_case_img, LV_OBJ_FLAG_HIDDEN);
    ui_unlock();
}

void ui_set_emotion(ui_emotion_t e)
{
    if (e >= UI_EMO_COUNT) return;
    ui_lock();
    s_emotion = e;
    ui_anim_emotion_changed(e);
    ui_unlock();
}

ui_emotion_t ui_get_emotion(void) { return s_emotion; }

esp_err_t ui_init(void)
{
    s_mutex = mutex_create();
    s_flush_sig = signal_create();
    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(BOARD_DISPLAY_W, BOARD_DISPLAY_H);
    size_t buf_bytes = BOARD_DISPLAY_W * UI_BUF_LINES * 2;
    void *b1 = mem_alloc_dma(buf_bytes);
    void *b2 = mem_alloc_dma(buf_bytes);
    if (!b1 || !b2) return ESP_ERR_NO_MEM;
    lv_display_set_buffers(s_disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    if (!bigimg_alloc(&s_scene_big, &scn_home_day) || !bigimg_alloc(&s_gugu_big[0], &spr_gugu_neutral) ||
        !bigimg_alloc(&s_gugu_big[1], &spr_gugu_neutral)) return ESP_ERR_NO_MEM;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_scene_img = lv_image_create(scr);                     // 464 px centred in 466
    lv_obj_remove_flag(s_scene_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_scene_img, 1, 1);
    show_scene_src(&scn_home_day);
    ui_scene_init(scr);                                     // window pane and props: above the backdrop, below the pigeon
    s_case_img = make_sprite(scr, &spr_case_down, 93 * UI_SPRITE_SCALE + 1, 62 * UI_SPRITE_SCALE + 1);
    ui_gugu_img = lv_image_create(scr);
    lv_obj_remove_flag(ui_gugu_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(ui_gugu_img, GUGU_BASE_X, GUGU_BASE_Y);
    s_beak_img = make_sprite(ui_gugu_img, NULL, 0, 0);
    s_eyes_img = make_sprite(ui_gugu_img, NULL, 0, 0);
    lv_obj_add_flag(s_beak_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_eyes_img, LV_OBJ_FLAG_HIDDEN);
    ui_scene_init_accessory();
    ui_scene_show_window(true);
    ui_anim_init(scr);

    if (!task_spawn("gugu_ui_main", ui_task, NULL, 12288, 4, 0)) return ESP_FAIL;   // LVGL draws deep: 8 KB left 612 B free (`mem`), PLAN.md 踩坑 35
    LOGI("ui ready: 2x%u B internal DMA draw buffers, %u KB of pre-scaled art in PSRAM", (unsigned)buf_bytes,
         (unsigned)((s_scene_big.dsc.data_size + 2 * s_gugu_big[s_gugu_slot].dsc.data_size) / 1024));
    return ESP_OK;
}

// ---- notice card ----
static lv_obj_t *s_notice, *s_notice_title, *s_notice_body;

void ui_show_notice(const char *title, const char *body)
{
    ui_lock();
    if (!s_notice) {
        s_notice = lv_obj_create(lv_screen_active());
        lv_obj_set_size(s_notice, BOARD_DISPLAY_W, BOARD_DISPLAY_H);
        lv_obj_center(s_notice);
        lv_obj_set_style_radius(s_notice, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_notice, lv_color_hex(0x1B2430), 0);
        lv_obj_set_style_bg_opa(s_notice, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_notice, 0, 0);
        lv_obj_set_style_pad_all(s_notice, 0, 0);
        lv_obj_remove_flag(s_notice, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_notice_title = lv_label_create(s_notice);
        lv_obj_set_style_text_color(s_notice_title, lv_color_hex(0xF6D27A), 0);
        lv_obj_set_style_text_font(s_notice_title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(s_notice_title, LV_TEXT_ALIGN_CENTER, 0);
        s_notice_body = lv_label_create(s_notice);
        lv_obj_set_style_text_color(s_notice_body, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_notice_body, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(s_notice_body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(s_notice_body, 12, 0);
    }
    lv_label_set_text(s_notice_title, title ? title : "");
    lv_label_set_text(s_notice_body, body ? body : "");
    lv_obj_align(s_notice_title, LV_ALIGN_CENTER, 0, -130);
    lv_obj_center(s_notice_body);
    lv_obj_remove_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_notice);
    ui_unlock();
}

void ui_hide_notice(void)
{
    ui_lock();
    if (s_notice) lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    ui_unlock();
}
