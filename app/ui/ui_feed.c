// ui_feed.c — the gift key's food (the yellow key, 礼物键): an item falls from above the circle and lands on the ground beside Gugu's feet with
// a small bounce (just behind the pigeon), on the row Gugu's feet stand on. Then Gugu handles it (instinct runs the steps,
// 2026-11-04, the owner: look at it, a face, then do something): picked up with a wing, held at the chest in both, held
// up beside the glasses for a look, bitten and eaten, hidden behind the back, or kicked out of the round window in an
// arc. Positions follow Gugu wherever it has walked.
#define LOG_TAG "ui"
#include "ui_internal.h"
#include "foundation/log.h"

#define FEED_GROUND (GUGU_FEET_ROW + 1)   // the item's bottom row lands on the row Gugu's feet stand on (and, away, the strip's edge)
#define FEED_FALL_MS 450
static lv_obj_t *s_item;
static int32_t s_land_y;
static void put_on_ground_layer(void);   // a held item back onto the scene (defined with the hand code below)

static void y_exec(void *obj, int32_t v) { lv_obj_set_y(obj, v); }

static void bounce(lv_anim_t *a)
{
    (void)a;
    lv_anim_t b; lv_anim_init(&b);
    lv_anim_set_var(&b, s_item);
    lv_anim_set_values(&b, s_land_y, s_land_y - 3 * UI_SPRITE_SCALE);
    lv_anim_set_duration(&b, 90);
    lv_anim_set_playback_duration(&b, 110);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&b, y_exec);
    lv_anim_start(&b);
}

void ui_feed_drop(int food, int land_x)
{
    if (food < 0 || food >= UI_FOOD_COUNT) return;
    ui_lock();
    if (!s_item) {
        s_item = lv_image_create(lv_obj_get_parent(ui_gugu_img));
        lv_image_set_pivot(s_item, 0, 0);
        lv_image_set_scale(s_item, 256 * UI_SPRITE_SCALE);
        lv_image_set_antialias(s_item, false);
        lv_obj_remove_flag(s_item, LV_OBJ_FLAG_CLICKABLE);
    }
    const lv_image_dsc_t *img = ui_food_sprites[food].img;
    lv_anim_delete(s_item, NULL);
    put_on_ground_layer();                                    // a new item falls on the scene, not in a hand
    lv_image_set_src(s_item, img);
    s_land_y = (FEED_GROUND - (int32_t)img->header.h) * UI_SPRITE_SCALE;
    int32_t top = -(int32_t)img->header.h * UI_SPRITE_SCALE;
    lv_obj_set_pos(s_item, land_x * UI_SPRITE_SCALE, top);
    lv_obj_remove_flag(s_item, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(s_item, lv_obj_get_index(ui_gugu_img));   // just behind the pigeon
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, s_item);
    lv_anim_set_values(&a, top, s_land_y);
    lv_anim_set_duration(&a, FEED_FALL_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, y_exec);
    lv_anim_set_completed_cb(&a, bounce);
    lv_anim_start(&a);
    ui_unlock();
}

void ui_feed_clear(void)
{
    ui_lock();
    if (s_item) { lv_anim_delete(s_item, NULL); lv_obj_add_flag(s_item, LV_OBJ_FLAG_HIDDEN); put_on_ground_layer(); }
    ui_unlock();
}

int ui_feed_width(int food) { return food >= 0 && food < UI_FOOD_COUNT ? (int)ui_food_sprites[food].img->header.w : 0; }

static void x_exec(void *obj, int32_t v) { lv_obj_set_x(obj, v); }
static void hide_done(lv_anim_t *a) { lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }
static void glide(int32_t x, int32_t y, uint32_t ms, bool hide)   // the item to (x, y) screen px in ms, gone after if asked
{
    lv_anim_delete(s_item, NULL);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, s_item); lv_anim_set_duration(&a, ms); lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_values(&a, lv_obj_get_x(s_item), x); lv_anim_set_exec_cb(&a, x_exec);
    if (hide) lv_anim_set_completed_cb(&a, hide_done);
    lv_anim_start(&a);
    lv_anim_t b; lv_anim_init(&b);
    lv_anim_set_var(&b, s_item); lv_anim_set_duration(&b, ms); lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_values(&b, lv_obj_get_y(s_item), y); lv_anim_set_exec_cb(&b, y_exec);
    lv_anim_start(&b);
}
static int32_t item_w(void) { const lv_image_dsc_t *d = lv_image_get_src(s_item); return d ? (int32_t)d->header.w * UI_SPRITE_SCALE : 0; }
static int32_t item_h(void) { const lv_image_dsc_t *d = lv_image_get_src(s_item); return d ? (int32_t)d->header.h * UI_SPRITE_SCALE : 0; }

// Held: the item is a child of the pigeon, centred on the hand of each pose (ui_anim tells us when the hand moves), so it
// walks with Gugu and is drawn in front of it; a pose without a hand (behind the back, or none at all) hides it.
static bool s_held;
static void put_on_ground_layer(void)
{
    if (!s_held || !s_item) return;
    lv_obj_set_parent(s_item, lv_obj_get_parent(ui_gugu_img));
    s_held = false;
}

void ui_feed_follow_hand(int hx, int hy)     // ui_anim, inside the UI lock
{
    if (!s_held || !s_item) return;
    if (hx < 0) { lv_obj_add_flag(s_item, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_item, LV_OBJ_FLAG_HIDDEN);
    glide(hx * UI_SPRITE_SCALE - item_w() / 2, hy * UI_SPRITE_SCALE - item_h() / 2, 120, false);
}

void ui_feed_take(void)
{
    ui_lock();
    if (s_item && !lv_obj_has_flag(s_item, LV_OBJ_FLAG_HIDDEN) && !s_held) {
        lv_anim_delete(s_item, NULL);
        int32_t x = lv_obj_get_x(s_item) - lv_obj_get_x(ui_gugu_img), y = lv_obj_get_y(s_item) - lv_obj_get_y(ui_gugu_img);
        lv_obj_set_parent(s_item, ui_gugu_img);
        lv_obj_set_pos(s_item, x, y);                          // where it lay, now in the pigeon's frame
        s_held = true;
        int hx, hy; ui_anim_hand_now(&hx, &hy);
        ui_feed_follow_hand(hx, hy);
    }
    ui_unlock();
}

// A kick: up and away along a parabola, out past the rim, then gone.
static int32_t s_kx0, s_ky0, s_kdir;
static void kick_exec(void *obj, int32_t v)  // v: 0 … 1000
{
    int32_t t = v, dx = s_kdir * 84 * UI_SPRITE_SCALE * t / 1000;
    int32_t up = 30 * UI_SPRITE_SCALE * 4 * t * (1000 - t) / 1000000;      // a hump 30 base pixels high at the middle
    int32_t fall = 40 * UI_SPRITE_SCALE * t * t / 1000000;                  // and a drop that wins at the end
    lv_obj_set_pos(obj, s_kx0 + dx, s_ky0 - up + fall);
}
void ui_feed_kick(int dir)
{
    ui_lock();
    if (s_item && !lv_obj_has_flag(s_item, LV_OBJ_FLAG_HIDDEN)) {
        lv_anim_delete(s_item, NULL);
        lv_obj_move_foreground(s_item);
        s_kx0 = lv_obj_get_x(s_item); s_ky0 = lv_obj_get_y(s_item); s_kdir = dir < 0 ? -1 : 1;
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, s_item); lv_anim_set_values(&a, 0, 1000); lv_anim_set_duration(&a, 700);
        lv_anim_set_exec_cb(&a, kick_exec); lv_anim_set_completed_cb(&a, hide_done);
        lv_anim_start(&a);
    }
    ui_unlock();
}
