// ui_scene.c — the object layer: props and the friend standing on the ground line of an away scene, the
// home's fittings (lamp, window, board, shelf, table) and the accessories Gugu wears. Objects are small
// LVGL images (built-in sprites from assets/, or generated .art sprites rendered into PSRAM) scaled ×4 like
// every other sprite; the time of day sends living things home at night, lights up lamps and dims the rest.
#define LOG_TAG "ui"
#include "ui_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "src/misc/cache/instance/lv_image_cache.h"   // lv_image_cache_drop is not re-exported by lvgl.h
#include <string.h>
#include <stdio.h>

typedef struct {
    ui_object_t def;
    lv_obj_t *img;
    lv_image_dsc_t dsc; uint8_t *buf; size_t cap;   // a generated sprite rendered at base resolution (RGB565A8)
    int w, h;                                       // base pixels
    bool on;                                        // lights
} slot_t;

static slot_t s_obj[UI_MAX_OBJECTS];
static int s_n;
static art_tod_t s_tod = ART_TOD_DAY;
static lv_obj_t *s_win_img, *s_acc_img, *s_bubble_img;
static lv_timer_t *s_bubble_timer; static bool s_bubble_up;
static char s_acc[16];
static bool s_lamp_on = true;
bool ui_scene_is_home = true;

// Home fittings baked into the room picture: base-grid boxes and what a touch does.
static const struct { const char *id; int x0, y0, x1, y1; ui_behave_t behave; } HOME[] = {
    { "lamp",   84, 2, 100, 22, UI_BEHAVE_TOGGLE },       // the room's art sits 6 rows higher than pixelgen's numbers (ROOM_LIFT)
    { "window", 17, 11, 51, 45, UI_BEHAVE_SOUND },
    { "shelf",  48, 0, 74, 14, UI_BEHAVE_SOUND },
    { "board",  82, 44, 106, 62, UI_BEHAVE_WIGGLE },
    { "table",  6, 56, 36, 86, UI_BEHAVE_SOUND },
};
#define HOME_N ((int)(sizeof HOME / sizeof HOME[0]))

static const lv_image_dsc_t *builtin_prop(const char *id)
{
    for (int i = 0; i < ui_prop_sprite_count; i++) if (!strcmp(ui_prop_sprites[i].id, id)) return ui_prop_sprites[i].img;
    return NULL;
}

static lv_obj_t *mk(lv_obj_t *parent)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_pivot(img, 0, 0);
    lv_image_set_scale(img, 256 * UI_SPRITE_SCALE);
    lv_image_set_antialias(img, false);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

void ui_scene_init(lv_obj_t *scr)
{
    s_win_img = mk(scr);
    lv_obj_set_pos(s_win_img, 17 * UI_SPRITE_SCALE + 1, 11 * UI_SPRITE_SCALE + 1);   // the window, lifted with the room
    lv_image_set_src(s_win_img, ui_window_sprites[ART_TOD_DAY]);
    for (int i = 0; i < UI_MAX_OBJECTS; i++) s_obj[i].img = mk(scr);
}

void ui_scene_init_accessory(void)   // after the pigeon exists: the overlay is its child; the bubble sits above it on the screen
{
    s_acc_img = mk(ui_gugu_img);
    s_bubble_img = mk(lv_obj_get_parent(ui_gugu_img));
    lv_image_set_src(s_bubble_img, &spr_bubble_setup);
}

// ---- the gear bubble: up at Gugu's upper right while nothing is set up, bobbing a pixel every 600 ms ----
#define BUBBLE_X 70                          // base pixels on the 116 grid: its trailing puffs end near the head
#define BUBBLE_Y 25                          // 31 before the 6-row lift (pixelgen ROOM_LIFT)
static void bubble_bob(lv_timer_t *t)
{
    (void)t;
    s_bubble_up = !s_bubble_up;
    lv_obj_set_pos(s_bubble_img, BUBBLE_X * UI_SPRITE_SCALE + 1, (BUBBLE_Y - (s_bubble_up ? 1 : 0)) * UI_SPRITE_SCALE + 1);
}

static ui_bubble_t s_bubble;
void ui_show_bubble(ui_bubble_t b)
{
    ui_lock();
    bool on = b != UI_BUBBLE_NONE;
    if (s_bubble_img && on && b != s_bubble) {
        lv_image_set_src(s_bubble_img, b == UI_BUBBLE_WIFI ? &spr_bubble_wifi : b == UI_BUBBLE_SERVICE ? &spr_bubble_service : b == UI_BUBBLE_HELLO ? &spr_bubble_hello :
                                       b == UI_BUBBLE_IDEA ? &spr_bubble_idea : b == UI_BUBBLE_STORY ? &spr_bubble_story : &spr_bubble_setup);
    }
    if (on) s_bubble = b;
    if (s_bubble_img) {
        if (on && lv_obj_has_flag(s_bubble_img, LV_OBJ_FLAG_HIDDEN)) {
            s_bubble_up = true; bubble_bob(NULL);
            lv_obj_remove_flag(s_bubble_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_bubble_img);
            if (!s_bubble_timer) s_bubble_timer = lv_timer_create(bubble_bob, 600, NULL);
            lv_timer_resume(s_bubble_timer);
        } else if (!on && !lv_obj_has_flag(s_bubble_img, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_bubble_img, LV_OBJ_FLAG_HIDDEN);
            if (s_bubble_timer) lv_timer_pause(s_bubble_timer);
        }
    }
    ui_unlock();
}

ui_bubble_t ui_bubble_shown(void)
{
    ui_lock();
    ui_bubble_t b = s_bubble_img && !lv_obj_has_flag(s_bubble_img, LV_OBJ_FLAG_HIDDEN) ? s_bubble : UI_BUBBLE_NONE;
    ui_unlock();
    return b;
}

bool ui_bubble_hit(int x, int y)          // the whole sprite, a little generous for small fingers
{
    ui_lock();
    bool hit = s_bubble_img && !lv_obj_has_flag(s_bubble_img, LV_OBJ_FLAG_HIDDEN) &&
               x >= (BUBBLE_X - 2) * UI_SPRITE_SCALE && x < (BUBBLE_X + (int)spr_bubble_setup.header.w + 2) * UI_SPRITE_SCALE &&
               y >= (BUBBLE_Y - 3) * UI_SPRITE_SCALE && y < (BUBBLE_Y + (int)spr_bubble_setup.header.h) * UI_SPRITE_SCALE;
    ui_unlock();
    return hit;
}

// ---- generated sprites ----
static bool render_generated(slot_t *s, const art_rgb_t *pal)
{
    const art_image_t *a = s->def.art;
    size_t n = (size_t)a->w * a->h;
    if (s->cap < n * 3) { mem_release(s->buf); s->buf = mem_alloc_big(n * 3); s->cap = s->buf ? n * 3 : 0; }
    if (!s->buf) return false;
    lv_image_cache_drop(&s->dsc);
    art_render_rgb565(a, pal, 1, (uint16_t *)s->buf, s->buf + n * 2);
    memset(&s->dsc, 0, sizeof s->dsc);
    s->dsc.header.magic = LV_IMAGE_HEADER_MAGIC; s->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    s->dsc.header.w = a->w; s->dsc.header.h = a->h; s->dsc.header.stride = a->w * 2;
    s->dsc.data_size = (uint32_t)n * 3; s->dsc.data = s->buf;
    return true;
}

// The look of one object at the current time of day: generated ones get the remapped palette, built-in ones a
// recolour; lights that are on keep their day colours; living things are out of sight at night.
static void apply_tod(slot_t *s)
{
    bool night = s_tod == ART_TOD_NIGHT;
    if (s->def.kind == UI_OBJ_LIVING && night) { lv_obj_add_flag(s->img, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s->img, LV_OBJ_FLAG_HIDDEN);
    bool lit = s->def.kind == UI_OBJ_LIGHT && s->on && s_tod != ART_TOD_DAY;
    art_tod_t tod = lit ? ART_TOD_DAY : s_tod;
    if (s->def.art) {
        art_rgb_t pal[ART_MAX_COLORS];
        art_palette_tod(s->def.art, tod, pal);
        render_generated(s, pal);
        lv_image_set_src(s->img, &s->dsc);
        lv_obj_set_style_image_recolor_opa(s->img, LV_OPA_TRANSP, 0);
    } else {
        lv_color_t c = tod == ART_TOD_NIGHT ? lv_color_make(18, 26, 64) : tod == ART_TOD_DUSK ? lv_color_make(255, 140, 60) : lv_color_make(255, 205, 175);
        lv_opa_t o = tod == ART_TOD_NIGHT ? LV_OPA_60 : tod == ART_TOD_DUSK ? LV_OPA_30 : tod == ART_TOD_SUNRISE ? LV_OPA_20 : LV_OPA_TRANSP;
        lv_obj_set_style_image_recolor(s->img, c, 0);
        lv_obj_set_style_image_recolor_opa(s->img, o, 0);
    }
    if (s->def.kind == UI_OBJ_LIGHT && !s->on && s_tod == ART_TOD_DAY) { lv_obj_set_style_image_recolor(s->img, lv_color_make(40, 40, 50), 0); lv_obj_set_style_image_recolor_opa(s->img, LV_OPA_40, 0); }
    lv_obj_invalidate(s->img);
}

void ui_scene_set_objects(const ui_object_t *objs, int n, art_tod_t tod)
{
    ui_lock();
    s_tod = tod;
    for (int i = 0; i < UI_MAX_OBJECTS; i++) lv_obj_add_flag(s_obj[i].img, LV_OBJ_FLAG_HIDDEN);
    s_n = 0;
    for (int i = 0; i < n && s_n < UI_MAX_OBJECTS; i++) {
        slot_t *s = &s_obj[s_n];
        s->def = objs[i]; s->on = true;
        const lv_image_dsc_t *src = NULL;
        if (s->def.art) { s->w = s->def.art->w; s->h = s->def.art->h; }
        else {
            src = builtin_prop(s->def.id);
            if (!src) { LOGW("object %s: no sprite", s->def.id); continue; }
            s->w = src->header.w; s->h = src->header.h;
            lv_image_set_src(s->img, src);
        }
        lv_obj_set_pos(s->img, s->def.x * UI_SPRITE_SCALE + 1, (s->def.y - s->h) * UI_SPRITE_SCALE + 1);
        apply_tod(s);
        s_n++;
    }
    ui_unlock();
}

void ui_scene_set_tod(art_tod_t tod)
{
    ui_lock();
    s_tod = tod;
    for (int i = 0; i < s_n; i++) apply_tod(&s_obj[i]);
    lv_image_set_src(s_win_img, ui_window_sprites[tod]);
    ui_unlock();
}

// ---- touching ----
static bool inside(int sx, int sy, int x0, int y0, int x1, int y1) { return sx >= x0 && sx < x1 && sy >= y0 && sy < y1; }

int ui_scene_hit(int sx, int sy)
{
    if (ui_scene_is_home) {
        for (int i = 0; i < HOME_N; i++)
            if (inside(sx, sy, HOME[i].x0 * UI_SPRITE_SCALE, HOME[i].y0 * UI_SPRITE_SCALE, HOME[i].x1 * UI_SPRITE_SCALE, HOME[i].y1 * UI_SPRITE_SCALE)) return 100 + i;
        return -1;
    }
    for (int i = s_n - 1; i >= 0; i--) {
        slot_t *s = &s_obj[i];
        if (lv_obj_has_flag(s->img, LV_OBJ_FLAG_HIDDEN)) continue;
        int x0 = s->def.x * UI_SPRITE_SCALE, y0 = (s->def.y - s->h) * UI_SPRITE_SCALE;
        if (inside(sx, sy, x0, y0, x0 + s->w * UI_SPRITE_SCALE, y0 + s->h * UI_SPRITE_SCALE)) return i;
    }
    return -1;
}

const char *ui_scene_object_id(int index)
{
    if (index >= 100 && index < 100 + HOME_N) return HOME[index - 100].id;
    return index >= 0 && index < s_n ? s_obj[index].def.id : "";
}

bool ui_scene_hit_gugu(int sx, int sy)
{
    return inside(sx, sy, GUGU_BASE_X, GUGU_BASE_Y, GUGU_BASE_X + 48 * UI_SPRITE_SCALE, GUGU_BASE_Y + 54 * UI_SPRITE_SCALE);
}

// a sideways shake (two swings) and a hop (two bounces), as LVGL animations on the object's position
static void shake_exec(void *obj, int32_t v) { lv_obj_t *img = obj; int base = (int)(intptr_t)lv_obj_get_user_data(img); lv_obj_set_x(img, base + 6 * lv_trigo_sin((int16_t)(v * 2)) / 32767); }
static void hop_exec(void *obj, int32_t v) { lv_obj_t *img = obj; int base = (int)(intptr_t)lv_obj_get_user_data(img); int s = lv_trigo_sin((int16_t)(v * 2)); if (s < 0) s = -s; lv_obj_set_y(img, base - 12 * s / 32767); }
static void shake_done(lv_anim_t *a) { lv_obj_t *img = a->var; lv_obj_set_x(img, (int)(intptr_t)lv_obj_get_user_data(img)); }
static void hop_done(lv_anim_t *a) { lv_obj_t *img = a->var; lv_obj_set_y(img, (int)(intptr_t)lv_obj_get_user_data(img)); }

static void animate(lv_obj_t *img, bool hop)
{
    lv_anim_delete(img, NULL);
    lv_obj_set_user_data(img, (void *)(intptr_t)(hop ? lv_obj_get_y(img) : lv_obj_get_x(img)));
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, img);
    lv_anim_set_values(&a, 0, 180);
    lv_anim_set_duration(&a, hop ? 500 : 380);
    lv_anim_set_exec_cb(&a, hop ? hop_exec : shake_exec);
    lv_anim_set_completed_cb(&a, hop ? hop_done : shake_done);
    lv_anim_start(&a);
}

ui_behave_t ui_scene_poke(int index)
{
    ui_behave_t b = UI_BEHAVE_SOUND;
    ui_lock();
    if (index >= 100 && index < 100 + HOME_N) {
        b = HOME[index - 100].behave;
        if (b == UI_BEHAVE_TOGGLE) { s_lamp_on = !s_lamp_on; ui_home_backdrop_refresh(); }
    } else if (index >= 0 && index < s_n) {
        slot_t *s = &s_obj[index];
        b = s->def.behave;
        switch (b) {
        case UI_BEHAVE_TOGGLE: s->on = !s->on; apply_tod(s); break;
        case UI_BEHAVE_WIGGLE: case UI_BEHAVE_WAVE: animate(s->img, false); break;
        case UI_BEHAVE_BOUNCE: animate(s->img, true); break;
        default: break;
        }
    }
    ui_unlock();
    return b;
}

// ---- the home's lamp and window ----
void ui_home_lamp(bool on) { ui_lock(); s_lamp_on = on; ui_home_backdrop_refresh(); ui_unlock(); }
bool ui_home_lamp_is_on(void) { return s_lamp_on; }
void ui_home_window(art_tod_t tod) { ui_scene_set_tod(tod); }
void ui_scene_show_window(bool show) { if (show) lv_obj_remove_flag(s_win_img, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_win_img, LV_OBJ_FLAG_HIDDEN); }

// ---- accessories ----
void ui_set_accessory(const char *id)
{
    ui_lock();
    snprintf(s_acc, sizeof s_acc, "%s", id ? id : "");
    ui_scene_refresh_accessory(ui_get_emotion());
    ui_unlock();
}

static void place_accessory(const ui_patch_t *glasses, int hat_y, int camera_y)
{
    if (!s_acc_img) return;
    const lv_image_dsc_t *src = NULL; int x = 0, y = 0;
    if (!strcmp(s_acc, "sun_hat")) { src = &spr_acc_sun_hat; x = 12; y = hat_y; }
    else if (!strcmp(s_acc, "sunglasses")) { src = glasses->img; x = glasses->x; y = glasses->y; }
    else if (!strcmp(s_acc, "camera")) { src = &spr_acc_camera; x = 18; y = camera_y; }
    if (!src) { lv_obj_add_flag(s_acc_img, LV_OBJ_FLAG_HIDDEN); return; }
    if (lv_image_get_src(s_acc_img) != src) lv_image_set_src(s_acc_img, src);
    lv_obj_set_pos(s_acc_img, x * UI_SPRITE_SCALE, y * UI_SPRITE_SCALE);
    lv_obj_remove_flag(s_acc_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_acc_img);
}

void ui_scene_refresh_accessory(ui_emotion_t e)
{
    if (e >= UI_EMOTION_COUNT) e = UI_EMO_NEUTRAL;
    place_accessory(&ui_sunglasses_patch[e], ui_hat_y[e], ui_camera_y[e]);
}

void ui_scene_refresh_accessory_pose(int frame)
{
    if (frame < 0 || frame >= UI_POSE_FRAME_COUNT) return;
    place_accessory(&ui_pose_sunglasses_patch[frame], ui_pose_hat_y[frame], ui_pose_camera_y[frame]);
}
