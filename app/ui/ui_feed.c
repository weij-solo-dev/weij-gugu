// ui_feed.c — the yellow key's food: an item falls from above the circle, lands on the ground beside Gugu's feet with
// a small bounce, and goes when Gugu pecks it up (instinct times the peck and the reaction). The item sits just behind
// the pigeon, so a hop or a dip never hides behind it.
#define LOG_TAG "ui"
#include "ui_internal.h"
#include "foundation/log.h"

#define FEED_GROUND 89            // base y where Gugu's feet stand: the item's bottom lands on it (95 before the 6-row lift)
#define FEED_FALL_MS 450
static lv_obj_t *s_item;
static int32_t s_land_y;

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
    if (s_item) { lv_anim_delete(s_item, NULL); lv_obj_add_flag(s_item, LV_OBJ_FLAG_HIDDEN); }
    ui_unlock();
}
