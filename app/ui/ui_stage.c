// ui_stage.c — the gift key's show (2026-11-06, the owner: "gugu 将变成歌手表演一段歌曲，对应场景也可以临时变化（比如
// disco 等）"). The stage takes the scene slot (scn_stage_disco, or scn_stage_night for the lullaby after dark) and a
// light layer moves on the beat: the dance floor's tiles change colour, the mirror ball turns, a pair of beams swaps
// bar by bar, sparkles jump about the walls, notes float up; a microphone stands in front of Gugu, who sways on the beat.
// Only small parts change each beat: a whole repaint costs 150–180 ms on the device (PLAN.md 踩坑 4), so the stage itself
// is put up once and taken down once, and a beat touches few areas. The floor is one pre-scaled picture repainted in
// place: as 36 tile objects plus the sparkles it made LVGL track more than its 32 dirty areas, and every beat became a
// whole-screen repaint of 300 ms on the device (PLAN.md 踩坑 55). The caller (instinct) puts its own scene back afterwards.
#define LOG_TAG "ui"
#include "ui_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "src/misc/cache/instance/lv_image_cache.h"   // lv_image_cache_drop is not re-exported by lvgl.h
#include <stdlib.h>
#include <string.h>

#define TILE_W 14
#define TILE_H 6
#define SPARKS 8
#define NOTES 3
#define S UI_SPRITE_SCALE
#define FLOOR_H (116 - UI_STAGE_FLOOR)
static lv_obj_t *s_floor, *s_ball, *s_beam[2], *s_spark[SPARKS], *s_mic, *s_note[NOTES];
static lv_image_dsc_t s_floor_dsc; static uint16_t *s_floor_px;   // the floor, pre-scaled (116 × FLOOR_H base pixels)
static ui_stage_t s_stage;
static int s_note_next;
static uint32_t s_rng = 0x9E3779B9u;
static uint32_t rnd(void) { uint32_t x = s_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return s_rng = x ? x : 0x9E3779B9u; }

static const uint32_t LIGHTS[] = { 0xFF5CA8, 0x4CD8F0, 0xFFD84A, 0x9A5CF0, 0x6CE08C };   // pink, cyan, yellow, purple, green
#define N_LIGHTS (sizeof LIGHTS / sizeof LIGHTS[0])
static const lv_image_dsc_t *const BEAMS[2][2] = { { &spr_stage_beam_pink, &spr_stage_beam_cyan }, { &spr_stage_beam_yel, &spr_stage_beam_purp } };
static const lv_image_dsc_t *const NOTE_IMG[NOTES] = { &spr_stage_note_yel, &spr_stage_note_pink, &spr_stage_note_cyan };

static uint16_t rgb565(uint32_t hex) { return (uint16_t)((((hex >> 16) & 0xFF) >> 3) << 11 | (((hex >> 8) & 0xFF) >> 2) << 5 | ((hex & 0xFF) >> 3)); }

// The dance floor for a beat: bricks of TILE_W × TILE_H base pixels in rows offset by half, most lit in turn, grout between.
static void floor_paint(int beat)
{
    if (!s_floor_px) return;
    const int W = 116 * S;
    const uint16_t grout = rgb565(0x100E22), dark = rgb565(0x3A3060);
    for (int by = 0; by < FLOOR_H; by++) {
        int r = by / (TILE_H + 1);
        bool row_grout = by % (TILE_H + 1) == TILE_H;
        for (int bx = 0; bx < 116; bx++) {
            int x = bx + 5 - (r % 2) * 7, c = x >= 0 ? x / (TILE_W + 1) : -1;
            uint16_t col;
            if (row_grout || x < 0 || x % (TILE_W + 1) == TILE_W) col = grout;
            else col = (c + r + beat) % 3 ? rgb565(LIGHTS[(c + r * 2 + beat * 3) % N_LIGHTS]) : dark;
            uint16_t *px = s_floor_px + (size_t)by * S * W + (size_t)bx * S;
            for (int k = 0; k < S; k++) px[k] = col;
        }
        uint16_t *row = s_floor_px + (size_t)by * S * W;
        for (int k = 1; k < S; k++) memcpy(row + (size_t)k * W, row, (size_t)W * 2);
    }
    lv_image_cache_drop(&s_floor_dsc);
    if (s_floor) lv_obj_invalidate(s_floor);
}

static lv_obj_t *box(lv_obj_t *scr, int w, int h)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}
// The stage's sprites are shown pre-scaled (ui_prescaled): no LVGL transform on any repaint.
static void set_src(lv_obj_t *img, const lv_image_dsc_t *src)
{
    const lv_image_dsc_t *big = ui_prescaled(src);
    if (big) { lv_image_set_scale(img, 256); lv_image_set_src(img, big); }
    else { lv_image_set_scale(img, 256 * S); lv_image_set_src(img, src); }   // no room in PSRAM: LVGL scales it
}
static lv_obj_t *sprite(lv_obj_t *scr, const lv_image_dsc_t *src)
{
    lv_obj_t *img = lv_image_create(scr);
    lv_image_set_pivot(img, 0, 0);
    lv_image_set_antialias(img, false);
    set_src(img, src);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

void ui_stage_init_back(lv_obj_t *scr)    // behind the pigeon: the floor, the ball, the beams, the sparkles
{
    s_floor_px = mem_alloc_big((size_t)116 * S * FLOOR_H * S * 2);
    memset(&s_floor_dsc, 0, sizeof s_floor_dsc);
    s_floor_dsc.header.magic = LV_IMAGE_HEADER_MAGIC; s_floor_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_floor_dsc.header.w = 116 * S; s_floor_dsc.header.h = FLOOR_H * S; s_floor_dsc.header.stride = 116 * S * 2;
    s_floor_dsc.data_size = (uint32_t)(116 * S * FLOOR_H * S * 2); s_floor_dsc.data = (const uint8_t *)s_floor_px;
    s_floor = lv_image_create(scr);
    lv_obj_remove_flag(s_floor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_floor, 1, (UI_STAGE_FLOOR + 1) * S + 1);
    lv_obj_add_flag(s_floor, LV_OBJ_FLAG_HIDDEN);
    if (s_floor_px) { floor_paint(0); lv_image_set_src(s_floor, &s_floor_dsc); }
    for (int i = 0; i < 2; i++) { s_beam[i] = sprite(scr, BEAMS[0][i]); lv_obj_set_pos(s_beam[i], (i ? 58 : 58 - 33) * S + 1, 22 * S + 1); }
    s_ball = sprite(scr, &spr_stage_ball_0); lv_obj_set_pos(s_ball, 50 * S + 1, 7 * S + 1);
    ui_prescaled(&spr_stage_ball_1);                                // every variant scaled now, not on a beat
    for (int p = 0; p < 2; p++) for (int i = 0; i < 2; i++) ui_prescaled(BEAMS[p][i]);
    for (int i = 0; i < SPARKS; i++) s_spark[i] = box(scr, 2 * S, 2 * S);
}

void ui_stage_init_front(lv_obj_t *scr)   // in front of the pigeon: the microphone, the notes
{
    s_mic = sprite(scr, &spr_stage_mic); lv_obj_set_pos(s_mic, 54 * S + 1, (UI_STAGE_FLOOR - 26) * S + 1);
    for (int i = 0; i < NOTES; i++) s_note[i] = sprite(scr, NOTE_IMG[i]);
}

static void show(lv_obj_t *o, bool on) { if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }

void ui_stage(ui_stage_t s)
{
    ui_lock();
    s_stage = s;
    bool disco = s == UI_STAGE_DISCO, on = s != UI_STAGE_NONE;
    if (on) ui_show_stage_scene(disco ? &scn_stage_disco : &scn_stage_night);
    if (disco) floor_paint(0);                                     // lit before the first beat
    show(s_floor, disco && s_floor_px);
    show(s_ball, disco); show(s_beam[0], disco); show(s_beam[1], disco); show(s_mic, disco);
    for (int i = 0; i < SPARKS; i++) show(s_spark[i], false);
    for (int i = 0; i < NOTES; i++) { lv_anim_delete(s_note[i], NULL); show(s_note[i], false); }
    if (!on) ui_walk_to(0, 0);
    ui_unlock();
    LOGI("stage: %s", disco ? "disco" : on ? "night" : "down");
}

ui_stage_t ui_stage_shown(void) { return s_stage; }

static void note_y(void *o, int32_t v) { lv_obj_set_y(o, v); }
static void note_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }
static void float_note(void)               // a note leaves Gugu's head and floats up
{
    lv_obj_t *n = s_note[s_note_next++ % NOTES];
    int x = (s_note_next & 1) ? 38 + (int)(rnd() % 6) : 72 + (int)(rnd() % 6), y = 44;
    lv_obj_set_pos(n, x * S + 1, y * S + 1);
    show(n, true);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, n); lv_anim_set_values(&a, y * S + 1, (y - 26) * S + 1);
    lv_anim_set_duration(&a, s_stage == UI_STAGE_NIGHT ? 2400 : 1300);
    lv_anim_set_exec_cb(&a, note_y); lv_anim_set_completed_cb(&a, note_done);
    lv_anim_start(&a);
}

void ui_stage_beat(int beat, int per_bar)
{
    if (s_stage == UI_STAGE_NONE) return;
    ui_lock();
    bool disco = s_stage == UI_STAGE_DISCO, bar = per_bar > 0 && beat % per_bar == 0;
    if (disco) {
        floor_paint(beat);                                         // one area for the whole floor
        if (!(beat & 1)) set_src(s_ball, (beat >> 1) & 1 ? &spr_stage_ball_1 : &spr_stage_ball_0);
        if (bar && (beat / per_bar) % 2 == 0) { int pair = (beat / per_bar / 2) & 1; set_src(s_beam[0], BEAMS[pair][0]); set_src(s_beam[1], BEAMS[pair][1]); }   // every other bar
        for (int k = 0; k < 2; k++) {                              // two sparkles jump each beat: the ball's light on the walls
            int i = (beat * 2 + k) % SPARKS, x = 4 + (int)(rnd() % 108), y = 4 + (int)(rnd() % 80);
            lv_obj_set_pos(s_spark[i], x * S + 1, y * S + 1);
            lv_obj_set_style_bg_color(s_spark[i], lv_color_hex(LIGHTS[(i + beat) % N_LIGHTS]), 0);
            show(s_spark[i], true);
        }
        if (!(beat & 1)) ui_walk_to((beat >> 1) & 1 ? 1 : -1, 120);   // a sway every other beat
    } else {                                                       // the lullaby: stars twinkle, a slow sway each bar
        for (int i = 0; i < SPARKS; i++) {
            if ((i + beat) % 3) continue;
            int x = 6 + (int)(rnd() % 104), y = 6 + (int)(rnd() % 56);
            lv_obj_set_pos(s_spark[i], x * S + 1, y * S + 1);
            lv_obj_set_style_bg_color(s_spark[i], lv_color_hex(i & 1 ? 0xFFF4C8 : 0xFFFFFF), 0);
            show(s_spark[i], true);
        }
        if (bar) ui_walk_to(((beat / per_bar) & 1) ? 1 : -1, 700);
    }
    if (bar) float_note();
    ui_unlock();
}

void ui_stage_cheer(void)                  // a tap during the show: the sparkles gather round Gugu, a jump
{
    if (s_stage == UI_STAGE_NONE) return;
    ui_lock();
    for (int i = 0; i < SPARKS; i++) {
        int x = 30 + (int)(rnd() % 56), y = 30 + (int)(rnd() % 40);
        lv_obj_set_pos(s_spark[i], x * S + 1, y * S + 1);
        lv_obj_set_style_bg_color(s_spark[i], lv_color_hex(LIGHTS[i % N_LIGHTS]), 0);
        show(s_spark[i], true);
    }
    float_note();
    ui_unlock();
    ui_play_action(UI_ACT_JUMP);
}
