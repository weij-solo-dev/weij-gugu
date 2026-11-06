// ui_anim.c — the expression animation table: beak sync while speaking, a blink when idle, one-shot actions (nod,
// wave, puff, coo, whistle), poses (looking aside, reading the paper, preening, stretching, walking, chewing, kicking, a
// sly glance, and the wings as hands: reaching, holding, lifting, sneaking, biting, hiding) and the walk to a spot, the
// listening ring. Runs on LVGL timers
// inside the UI task; other modules only poke the small API in ui.h.
//
// Everything that moves often is small: the beak (flapping with the voice) and the eyelids (a blink) are patches drawn
// over the pre-scaled base sprite, so one frame repaints a few hundred pixels. The whole pigeon is repainted on an
// emotion change, a pose frame and the one-shot actions. The body keeps still when idle: the breathing bob (one base
// pixel every 1.2 s, forever) is gone (2026-11-04, the owner: "一直上下，这样不好"); idle life is the poses instead.
#define LOG_TAG "ui"
#include "ui_internal.h"
#include "board/board.h"
#include "foundation/log.h"
#include "foundation/lipsync.h"
#include <stdlib.h>
#include <math.h>

static lv_obj_t *s_ring, *s_dot;
static ui_emotion_t s_emotion = UI_EMO_NEUTRAL;
static bool s_mouth_open, s_idle = true, s_blinking;
static uint32_t s_wave_until, s_puff_until;
static lv_timer_t *s_blink_timer, *s_mouth_timer, *s_pose_timer, *s_dot_timer;
static uint8_t s_level;

static int rest_y(void) { return GUGU_BASE_Y; }
static int s_dx;                                   // where Gugu stands, base pixels from home (ui_walk_to)
static int home_x(void) { return GUGU_BASE_X + s_dx * UI_SPRITE_SCALE; }

// ---- poses: two frames at most, alternating every ms; the newspaper goes with READ ----
static ui_pose_t s_pose;
static uint8_t s_pose_phase;
static uint16_t s_pose_ticks;
static lv_obj_t *s_news;
static const struct { int8_t f0, f1; uint16_t ms; } POSE[UI_POSE_COUNT] = {
    [UI_POSE_NONE]    = { -1, -1, 0 },
    [UI_POSE_LOOK_L]  = { UI_PF_LOOK_L, -1, 0 },          [UI_POSE_LOOK_R]  = { UI_PF_LOOK_R, -1, 0 },
    [UI_POSE_READ]    = { UI_PF_READ_L, UI_PF_READ_R, 900 },
    [UI_POSE_CHEW]    = { UI_PF_CHEW_A, UI_PF_CHEW_B, 160 },
    [UI_POSE_PREEN]   = { UI_PF_PREEN_A, UI_PF_PREEN_B, 340 },
    [UI_POSE_STRETCH] = { UI_PF_STRETCH, -1, 0 },
    [UI_POSE_WALK_L]  = { UI_PF_WALK_L_A, UI_PF_WALK_L_B, 130 }, [UI_POSE_WALK_R] = { UI_PF_WALK_R_A, UI_PF_WALK_R_B, 130 },
    [UI_POSE_KICK_L]  = { UI_PF_KICK_L, -1, 0 },          [UI_POSE_KICK_R]  = { UI_PF_KICK_R, -1, 0 },
    [UI_POSE_SLY]     = { UI_PF_SLY_L, UI_PF_SLY_R, 450 },
    [UI_POSE_REACH_L] = { UI_PF_REACH_L, -1, 0 },         [UI_POSE_REACH_R] = { UI_PF_REACH_R, -1, 0 },
    [UI_POSE_HOLD_L]  = { UI_PF_HOLD_L, -1, 0 },          [UI_POSE_HOLD_R]  = { UI_PF_HOLD_R, -1, 0 },
    [UI_POSE_LIFT_L]  = { UI_PF_LIFT_L, -1, 0 },          [UI_POSE_LIFT_R]  = { UI_PF_LIFT_R, -1, 0 },
    [UI_POSE_SNEAK_L] = { UI_PF_SNEAK_L_A, UI_PF_SNEAK_L_B, 420 }, [UI_POSE_SNEAK_R] = { UI_PF_SNEAK_R_A, UI_PF_SNEAK_R_B, 420 },
    [UI_POSE_BITE_L]  = { UI_PF_BITE_L, -1, 0 },          [UI_POSE_BITE_R]  = { UI_PF_BITE_R, -1, 0 },
    [UI_POSE_HIDE_L]  = { UI_PF_HIDE_L, -1, 0 },          [UI_POSE_HIDE_R]  = { UI_PF_HIDE_R, -1, 0 },
};
static int pose_frame(void) { return s_pose_phase && POSE[s_pose].f1 >= 0 ? POSE[s_pose].f1 : POSE[s_pose].f0; }
static void news_show(bool on)
{
    if (!s_news) return;
    if (!on) { lv_obj_add_flag(s_news, LV_OBJ_FLAG_HIDDEN); return; }
    const lv_image_dsc_t *src = s_pose_ticks % 6 == 5 ? &spr_news_turn : &spr_news;   // a page turned now and then
    if (lv_image_get_src(s_news) != src) lv_image_set_src(s_news, src);
    lv_obj_remove_flag(s_news, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_news);
}

static void refresh_sprite(void)
{
    if (s_pose != UI_POSE_NONE) {                          // a pose: its frame, no patches, the trip's accessory on its head
        int f = pose_frame();
        ui_show_base(ui_pose_sprites[f]); ui_show_beak(NULL); ui_show_eyes(NULL);
        news_show(s_pose == UI_POSE_READ);
        ui_scene_refresh_accessory_pose(f);
        ui_feed_follow_hand(ui_pose_hand[f][0], ui_pose_hand[f][1]);   // whatever it holds goes with the hand
        ui_show_grip(&ui_pose_grip[f]);                    // and the hand's tip is over it: held, not stuck on
        return;
    }
    news_show(false);
    ui_feed_follow_hand(-1, -1);
    ui_show_grip(NULL);
    uint32_t now = lv_tick_get();
    const lv_image_dsc_t *base;
    const ui_patch_t *beak = NULL, *eyes = NULL;
    if (s_puff_until && now < s_puff_until) base = s_mouth_open ? &spr_gugu_puff_open : &spr_gugu_puff;
    else if (s_wave_until && now < s_wave_until) base = s_mouth_open ? &spr_gugu_wave_open : &spr_gugu_wave;
    else {
        base = ui_emotion_sprites[s_emotion];
        if (s_mouth_open) {
            if (ui_beak_patch[s_emotion].img) beak = &ui_beak_patch[s_emotion];
            else base = ui_emotion_sprites_open[s_emotion];        // no patch for this face: swap the sprite
        }
        if (s_blinking && ui_blink_patch.img && ui_blink_fits[s_emotion]) eyes = &ui_blink_patch;
    }
    ui_show_base(base);
    ui_show_beak(beak);
    ui_show_eyes(eyes);
    ui_scene_refresh_accessory(base == &spr_gugu_puff || base == &spr_gugu_puff_open ? UI_EMO_PUFF : base == &spr_gugu_wave || base == &spr_gugu_wave_open ? UI_EMO_WAVE : s_emotion);
}

static void jitter_exec(void *obj, int32_t v) { lv_obj_set_x(obj, home_x() + 2 * UI_SPRITE_SCALE * lv_trigo_sin((int16_t)(v * 20)) / 32767); }
static void sway_exec(void *obj, int32_t v) { lv_obj_set_x(obj, home_x() + 6 * UI_SPRITE_SCALE * lv_trigo_sin((int16_t)(v * 2)) / 32767); }
static void headshake_exec(void *obj, int32_t v) { lv_obj_set_x(obj, home_x() + 4 * UI_SPRITE_SCALE * lv_trigo_sin((int16_t)(v * 3)) / 32767); }   // three swings: no, no, no
static void x_done(lv_anim_t *a) { (void)a; lv_obj_set_x(ui_gugu_img, home_x()); }
static void wobble(lv_anim_exec_xcb_t exec, uint32_t ms)
{
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, ui_gugu_img);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_completed_cb(&a, x_done);
    lv_anim_start(&a);
}
static void bob(int32_t depth, uint32_t ms, uint32_t repeat);
static void hop(int32_t height, uint32_t ms);
static bool s_ball;

// A change of mood is felt as well as seen: a small movement goes with the new face.
void ui_anim_emotion_changed(ui_emotion_t e)
{
    s_emotion = e; refresh_sprite();
    if (s_ball) return;
    switch (e) {
    case UI_EMO_LAUGHING: bob(2 * UI_SPRITE_SCALE, 110, 3); break;
    case UI_EMO_HAPPY: case UI_EMO_PROUD: hop(5 * UI_SPRITE_SCALE, 260); break;
    case UI_EMO_SURPRISED: hop(8 * UI_SPRITE_SCALE, 220); break;
    case UI_EMO_SCARED: wobble(jitter_exec, 900); break;
    case UI_EMO_DIZZY: wobble(sway_exec, 2000); break;
    case UI_EMO_ANGRY: bob(3 * UI_SPRITE_SCALE, 120, 0); break;
    case UI_EMO_URGE: bob(2 * UI_SPRITE_SCALE, 100, 1); break;                          // eyes back on the bubble: two quick stamps, "that!"
    default: break;
    }
}

// ---- mouth: the beak follows the voice (foundation/lipsync.h), decided every 40 ms ----
static lipsync_t s_lips;
static void mouth_timer_cb(lv_timer_t *t)
{
    (void)t;
    bool open = lipsync_step(&s_lips, s_level, lv_tick_get());
    if (open != s_mouth_open) { s_mouth_open = open; refresh_sprite(); }
    if (s_wave_until && lv_tick_get() >= s_wave_until) { s_wave_until = 0; refresh_sprite(); }
    if (s_puff_until && lv_tick_get() >= s_puff_until) { s_puff_until = 0; refresh_sprite(); }
}

void ui_set_mouth(uint8_t level) { s_level = level; }

// ---- blink: only faces whose eyes match the neutral eyelid patch blink ----
static void blink_close_cb(lv_timer_t *t);
static void blink_open_cb(lv_timer_t *t)
{
    (void)t;
    s_blinking = false; refresh_sprite();
    lv_timer_set_period(s_blink_timer, 2500 + (uint32_t)(rand() % 3000));
    lv_timer_set_cb(s_blink_timer, blink_close_cb);
}
static void blink_close_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_idle || s_mouth_open || s_pose != UI_POSE_NONE || !ui_blink_fits[s_emotion] || (s_puff_until && lv_tick_get() < s_puff_until) ||
        (s_wave_until && lv_tick_get() < s_wave_until)) return;
    s_blinking = true; refresh_sprite();
    lv_timer_set_period(s_blink_timer, 120);
    lv_timer_set_cb(s_blink_timer, blink_open_cb);
}

// ---- poses and the walk ----
static void pose_cb(lv_timer_t *t) { (void)t; s_pose_phase ^= 1; s_pose_ticks++; refresh_sprite(); }

void ui_pose(ui_pose_t p)
{
    if (p >= UI_POSE_COUNT) p = UI_POSE_NONE;
    ui_lock();
    if (p != s_pose) {
        s_pose = p; s_pose_phase = 0; s_pose_ticks = 0;
        if (p != UI_POSE_NONE && POSE[p].ms) { lv_timer_set_period(s_pose_timer, POSE[p].ms); lv_timer_reset(s_pose_timer); lv_timer_resume(s_pose_timer); }
        else lv_timer_pause(s_pose_timer);
        s_blinking = false;
        refresh_sprite();
    }
    ui_unlock();
}
ui_pose_t ui_get_pose(void) { return s_pose; }

static void walk_exec(void *obj, int32_t v) { lv_obj_set_x(obj, v); }
static void walk_done(lv_anim_t *a) { (void)a; lv_obj_set_x(ui_gugu_img, home_x()); }
void ui_walk_to(int dx, uint32_t ms)
{
    ui_lock();
    int32_t from = lv_obj_get_x(ui_gugu_img);
    s_dx = dx;
    lv_anim_delete(ui_gugu_img, walk_exec);
    if (!ms || from == home_x()) lv_obj_set_x(ui_gugu_img, home_x());
    else {
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, ui_gugu_img);
        lv_anim_set_values(&a, from, home_x());
        lv_anim_set_duration(&a, ms);
        lv_anim_set_exec_cb(&a, walk_exec);
        lv_anim_set_completed_cb(&a, walk_done);
        lv_anim_start(&a);
    }
    ui_unlock();
}
int ui_gugu_dx(void) { return s_dx; }
int ui_pose_hand_x(ui_pose_t p) { return p > UI_POSE_NONE && p < UI_POSE_COUNT && POSE[p].f0 >= 0 ? ui_pose_hand[POSE[p].f0][0] : -1; }
void ui_anim_hand_now(int *hx, int *hy)
{
    int f = s_pose != UI_POSE_NONE ? pose_frame() : -1;
    *hx = f >= 0 ? ui_pose_hand[f][0] : -1; *hy = f >= 0 ? ui_pose_hand[f][1] : -1;
}

void ui_set_idle(bool on)
{
    s_idle = on;
    if (!on && s_blinking) { s_blinking = false; refresh_sprite(); }
}

// ---- one-shot actions ----
static void bob_done(lv_anim_t *a);
// a hop: up and back down along a half sine
static void hop_exec(void *obj, int32_t v) { lv_obj_set_y(obj, rest_y() - v); }
static void hop(int32_t height, uint32_t ms)
{
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, ui_gugu_img);
    lv_anim_set_values(&a, 0, height);
    lv_anim_set_duration(&a, ms / 2); lv_anim_set_playback_duration(&a, ms / 2);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, hop_exec);
    lv_anim_set_completed_cb(&a, bob_done);
    lv_anim_start(&a);
}
// a loop around the window: the pigeon (wings up) follows a circle and lands back on its spot
static void fly_exec(void *obj, int32_t v)
{
    int32_t cx = home_x(), cy = rest_y(), r = 36 * UI_SPRITE_SCALE;
    int32_t s = lv_trigo_sin((int16_t)v), c = lv_trigo_cos((int16_t)v);
    lv_obj_set_pos(obj, cx - r * s / 32767, cy - r * (32767 - c) / 32767 / 2);
}
static void fly_done(lv_anim_t *a) { (void)a; lv_obj_set_pos(ui_gugu_img, home_x(), rest_y()); s_wave_until = 0; refresh_sprite(); }
static void fly(uint32_t ms)
{
    s_wave_until = lv_tick_get() + ms; refresh_sprite();
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, ui_gugu_img);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, fly_exec);
    lv_anim_set_completed_cb(&a, fly_done);
    lv_anim_start(&a);
}
static void bob_exec(void *obj, int32_t v) { lv_obj_set_y(obj, rest_y() + v); }
static void bob_done(lv_anim_t *a) { (void)a; lv_obj_set_y(ui_gugu_img, rest_y()); }
static void bob(int32_t depth, uint32_t ms, uint32_t repeat)
{
    lv_anim_t n; lv_anim_init(&n);
    lv_anim_set_var(&n, ui_gugu_img); lv_anim_set_exec_cb(&n, bob_exec);
    lv_anim_set_values(&n, 0, depth); lv_anim_set_duration(&n, ms); lv_anim_set_playback_duration(&n, ms);
    lv_anim_set_repeat_count(&n, repeat); lv_anim_set_completed_cb(&n, bob_done);
    lv_anim_start(&n);
}

void ui_play_action(ui_action_t a)
{
    ui_lock();
    switch (a) {
    case UI_ACT_NOD: bob(2 * UI_SPRITE_SCALE, 140, 2); break;
    case UI_ACT_WAVE: s_wave_until = lv_tick_get() + 900; refresh_sprite(); break;
    case UI_ACT_PUFF: s_puff_until = lv_tick_get() + 1500; refresh_sprite(); break;
    case UI_ACT_COO: case UI_ACT_WHISTLE: bob(UI_SPRITE_SCALE, 90, 0); break;   // a quick squash; the sound is the caller's
    case UI_ACT_PECK: bob(3 * UI_SPRITE_SCALE, 110, 1); break;                    // two quick dips
    case UI_ACT_JUMP: hop(10 * UI_SPRITE_SCALE, 320); break;
    case UI_ACT_FLY: fly(1400); break;
    case UI_ACT_SHAKE_HEAD: wobble(headshake_exec, 700); break;
    }
    ui_unlock();
}

// ---- listening: a static ring at the rim plus a small dot that pulses at the top ----
static void dot_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_obj_has_flag(s_dot, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_listening(bool on)
{
    ui_lock();
    if (on) {
        lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(s_dot_timer);
    } else {
        lv_timer_pause(s_dot_timer);
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    }
    ui_unlock();
}

// ---- the ball: a timer moves the puffed-up pigeon inside the circle, reflecting it off the rim ----
// Tuned on the owner's word (2026-11-03): the earlier version's *later* phase felt right — about 20 px per
// 25 ms tick, a short buzz at every wall — but it began slowly; a faster, spinning version felt worse (the
// rotation is a software transform on the device and stutters). So: that speed from the very first frame,
// no rotation, kicks keep it in the same band.
static lv_timer_t *s_ball_timer; static float s_bx, s_by, s_bvx, s_bvy;
#define BALL_R      (24 * UI_SPRITE_SCALE)       // half the puffed sprite, roughly
#define WIN_R       (BOARD_DISPLAY_W / 2)
#define BALL_TICK   25                           // ms per step
#define BALL_SPEED  20.0f                        // px per step at the start and after each kick (~800 px/s)
#define BALL_MAX    23.0f

static void ball_cb(lv_timer_t *t)
{
    (void)t;
    s_bx += s_bvx; s_by += s_bvy;
    float dx = s_bx - WIN_R, dy = s_by - WIN_R, d = sqrtf(dx * dx + dy * dy), lim = (float)(WIN_R - BALL_R);
    if (d > lim) {                               // reflect the velocity about the rim's normal, nudge back inside
        float nx = dx / d, ny = dy / d, dot = s_bvx * nx + s_bvy * ny;
        s_bvx -= 2 * dot * nx; s_bvy -= 2 * dot * ny;
        s_bx = WIN_R + nx * (lim - 1); s_by = WIN_R + ny * (lim - 1);
        s_bvx *= 0.96f; s_bvy *= 0.96f;
        board_vibrate(20);
    }
    lv_obj_set_pos(ui_gugu_img, (int)s_bx - BALL_R, (int)s_by - BALL_R);
}

static void ball_aim(float speed)               // keep the direction, set the speed, add a little randomness
{
    float sp = sqrtf(s_bvx * s_bvx + s_bvy * s_bvy);
    if (sp < 1) { s_bvx = 0.65f; s_bvy = -0.76f; sp = 1; }
    s_bvx = s_bvx / sp * speed + (float)(lv_rand(0, 6) - 3);
    s_bvy = s_bvy / sp * speed + (float)(lv_rand(0, 6) - 3);
}

void ui_bounce_start(void)
{
    ui_lock();
    if (!s_ball) {
        s_ball = true;
        s_puff_until = 0xFFFFFFFFu; refresh_sprite();
        s_bx = GUGU_BASE_X + BALL_R; s_by = GUGU_BASE_Y + BALL_R;
        s_bvx = lv_rand(0, 1) ? 0.65f : -0.65f; s_bvy = -0.76f;   // up and to one side
        ball_aim(BALL_SPEED);                    // full speed from the first frame
        if (!s_ball_timer) s_ball_timer = lv_timer_create(ball_cb, BALL_TICK, NULL);
        lv_timer_resume(s_ball_timer);
    }
    ui_unlock();
}

void ui_bounce_kick(int intensity)
{
    ui_lock();
    if (s_ball) {
        float target = BALL_SPEED + 3.0f * (float)intensity / 255.0f;   // a harder shake, a touch faster
        ball_aim(target > BALL_MAX ? BALL_MAX : target);
    }
    ui_unlock();
}

bool ui_bounce_stop(void)
{
    ui_lock();
    bool was = s_ball;
    if (s_ball) {
        s_ball = false;
        lv_timer_pause(s_ball_timer);
        s_puff_until = 0;
        lv_obj_set_pos(ui_gugu_img, GUGU_BASE_X, rest_y());
        refresh_sprite();
    }
    ui_unlock();
    return was;
}

void ui_anim_init(lv_obj_t *scr)
{
    s_ring = lv_arc_create(scr);
    lv_obj_set_size(s_ring, BOARD_DISPLAY_W - 6, BOARD_DISPLAY_H - 6);
    lv_obj_center(s_ring);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_value(s_ring, 100);
    lv_arc_set_range(s_ring, 0, 100);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(0xF6D27A), LV_PART_INDICATOR);
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);

    s_dot = lv_obj_create(scr);
    lv_obj_set_size(s_dot, 5 * UI_SPRITE_SCALE, 5 * UI_SPRITE_SCALE);
    lv_obj_set_pos(s_dot, (BOARD_DISPLAY_W - 5 * UI_SPRITE_SCALE) / 2, 4 * UI_SPRITE_SCALE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xF6D27A), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_pad_all(s_dot, 0, 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

    s_blink_timer = lv_timer_create(blink_close_cb, 3000, NULL);
    lipsync_init(&s_lips);
    s_mouth_timer = lv_timer_create(mouth_timer_cb, 40, NULL);
    s_pose_timer = lv_timer_create(pose_cb, 300, NULL);
    lv_timer_pause(s_pose_timer);
    s_news = lv_image_create(ui_gugu_img);                  // the paper, in front of the belly while reading
    lv_image_set_src(s_news, &spr_news);
    lv_image_set_pivot(s_news, 0, 0);
    lv_image_set_scale(s_news, 256 * UI_SPRITE_SCALE);
    lv_image_set_antialias(s_news, false);
    lv_obj_remove_flag(s_news, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_news, UI_NEWS_X * UI_SPRITE_SCALE, UI_NEWS_Y * UI_SPRITE_SCALE);
    lv_obj_add_flag(s_news, LV_OBJ_FLAG_HIDDEN);
    s_dot_timer = lv_timer_create(dot_cb, 300, NULL);
    lv_timer_pause(s_dot_timer);
    refresh_sprite();
}
