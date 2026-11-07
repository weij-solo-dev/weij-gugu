// ui.h — LVGL scenes and the expression animation table. The UI owns the LVGL task; other modules
// change what is shown through this API and never touch LVGL objects directly.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "art/art.h"

typedef enum {   // order matches ui_emotion_sprites (generated): the 12 emotions + puff + wave + urge (worried, eyes on the bubble / on you) + peek (a calm look at the bubble) + eyeroll (无语)
    UI_EMO_NEUTRAL = 0, UI_EMO_HAPPY, UI_EMO_LAUGHING, UI_EMO_SURPRISED, UI_EMO_CURIOUS,
    UI_EMO_SHY, UI_EMO_SAD, UI_EMO_SLEEPY, UI_EMO_ANGRY, UI_EMO_SCARED, UI_EMO_PROUD, UI_EMO_DIZZY,
    UI_EMO_PUFF, UI_EMO_WAVE, UI_EMO_URGE, UI_EMO_URGE_YOU, UI_EMO_PEEK, UI_EMO_EYEROLL, UI_EMO_COUNT
} ui_emotion_t;

typedef enum { UI_SCENE_HOME, UI_SCENE_AWAY } ui_scene_t;

esp_err_t ui_init(void);
// Serialise access to LVGL from other tasks.
void ui_lock(void);
void ui_unlock(void);

void ui_show_scene(ui_scene_t scene, bool night);
// A generated backdrop (116 grid) with the palette for the time of day takes the scene slot; ui_show_scene() brings the built-in art back.
void ui_show_backdrop(const art_image_t *img, const art_rgb_t pal[ART_MAX_COLORS]);
void ui_set_emotion(ui_emotion_t e);
ui_emotion_t ui_get_emotion(void);

// ---- animation layer (ui_anim.c) ----
typedef enum { UI_ACT_NOD, UI_ACT_WAVE, UI_ACT_COO, UI_ACT_WHISTLE, UI_ACT_PUFF, UI_ACT_PECK, UI_ACT_JUMP, UI_ACT_FLY, UI_ACT_SHAKE_HEAD } ui_action_t;   // fly: a loop around the window; shake_head: "no"
void ui_play_action(ui_action_t a);          // short one-shot animation on top of the current emotion
// ---- poses: the body doing something (idle life, the food), over the current expression until the next pose ----
typedef enum {
    UI_POSE_NONE = 0,
    UI_POSE_LOOK_L, UI_POSE_LOOK_R,              // eyes on something to one side
    UI_POSE_READ,                                // the newspaper, eyes along the lines, a page turned now and then
    UI_POSE_CHEW,                                // eating: the beak works
    UI_POSE_PREEN,                               // grooming the lifted wing
    UI_POSE_STRETCH,                             // wings up, a yawn
    UI_POSE_WALK_L, UI_POSE_WALK_R,              // stepping, the feet in turn
    UI_POSE_KICK_L, UI_POSE_KICK_R,              // a foot swung out to that side
    UI_POSE_SLY,                                 // 偷摸: glancing left, then right
    // the wing on that side as a hand (2026-11-04, the owner: "它翅膀可以当做手，应该是用手拾起"); a taken item sits in it
    UI_POSE_REACH_L, UI_POSE_REACH_R,            // bowed, the hand at the ground beside the feet
    UI_POSE_HOLD_L, UI_POSE_HOLD_R,              // the item in front of the chest, eyes on it
    UI_POSE_LIFT_L, UI_POSE_LIFT_R,              // held up by the glasses for a good look
    UI_POSE_SNEAK_L, UI_POSE_SNEAK_R,            // held close, glancing left and right
    UI_POSE_BITE_L, UI_POSE_BITE_R,              // up to the open beak
    UI_POSE_HIDE_L, UI_POSE_HIDE_R,              // the hand behind the back: the item out of sight
    UI_POSE_BOOK,                                // a story: the picture book held like the paper, a page turned now and then
    UI_POSE_COUNT
} ui_pose_t;
void ui_pose(ui_pose_t p);                   // UI_POSE_NONE: back to the expression
ui_pose_t ui_get_pose(void);
void ui_walk_to(int dx, uint32_t ms);        // stand dx base pixels from home (0 = home), getting there in ms (0: at once)
int  ui_gugu_dx(void);                       // where Gugu stands now, base pixels from home
int  ui_pose_hand_x(ui_pose_t p);             // where that pose's hand is, base x within the 48-wide sprite; -1 none
// ---- the gift key's show (ui_stage.c): a stage over the scene, lights on the beat ----
typedef enum { UI_STAGE_NONE = 0, UI_STAGE_DISCO, UI_STAGE_NIGHT } ui_stage_t;
void ui_stage(ui_stage_t s);                 // put the stage up (NONE takes it down: the caller puts its scene back)
ui_stage_t ui_stage_shown(void);
void ui_stage_beat(int beat, int per_bar);   // the lights and the sway on a beat (beat 0 is the first)
void ui_stage_cheer(void);                   // a tap during the show: sparkles round Gugu, a jump
void ui_set_mouth(uint8_t level);            // 0–255 output loudness → beak open/closed while speaking
void ui_set_listening(bool on);              // thin pulsing ring while the key is held
typedef enum { UI_BUBBLE_NONE = 0, UI_BUBBLE_SETUP, UI_BUBBLE_WIFI, UI_BUBBLE_SERVICE, UI_BUBBLE_HELLO, UI_BUBBLE_IDEA, UI_BUBBLE_STORY } ui_bubble_t;   // gear, Wi-Fi crossed out, ⚠; "Hi!", a light bulb, a moon and a book (bedtime)
void ui_show_bubble(ui_bubble_t b);          // the thought bubble at Gugu's upper right, bobbing gently (a call could not start)
bool ui_bubble_hit(int x, int y);            // a touch on the bubble while it is up
ui_bubble_t ui_bubble_shown(void);          // the bubble on screen now, or UI_BUBBLE_NONE

// ---- the yellow key's food (ui_feed.c): one item falls from the top and lands beside Gugu's feet ----
void ui_feed_drop(int food, int land_x);     // food: an index into ui_food_sprites; land_x: the item's left edge, base pixels (116 grid)
void ui_feed_take(void);                     // into Gugu's hand: from now on it sits in the hand of each pose (none: out of sight)
void ui_feed_kick(int dir);                  // off the ground in an arc, out of the round window (dir -1 left, +1 right)
void ui_feed_clear(void);                    // eaten, or otherwise gone
int  ui_feed_width(int food);                // the item's width in base pixels
void ui_set_idle(bool on);                   // blink when nobody is talking to Gugu (the body keeps still: no breathing bob)

// ---- the object layer (ui_scene.c): props and the friend on the ground line, the home's fittings, accessories ----
typedef enum { UI_OBJ_STATIC = 0, UI_OBJ_LIVING, UI_OBJ_LIGHT } ui_obj_kind_t;
typedef enum { UI_BEHAVE_WIGGLE = 0, UI_BEHAVE_TOGGLE, UI_BEHAVE_WAVE, UI_BEHAVE_BOUNCE, UI_BEHAVE_SOUND } ui_behave_t;
#define UI_MAX_OBJECTS 6
typedef struct {
    char id[24];               // catalogue id ("lamp"), "friend", …
    int x, y;                  // base grid: the left edge and the row the object stands on (its bottom)
    ui_obj_kind_t kind;
    ui_behave_t behave;
    const art_image_t *art;    // a generated sprite the caller keeps alive, or NULL for the built-in sprite of that id
} ui_object_t;
void ui_scene_set_objects(const ui_object_t *objs, int n, art_tod_t tod);   // replaces the layer (away scenes; n = 0 clears it)
void ui_scene_set_tod(art_tod_t tod);           // living things leave at night, lights come on, the rest dims; the window pane follows
int  ui_scene_hit(int sx, int sy);              // screen px → an object index (home fittings are 100+), or -1
const char *ui_scene_object_id(int index);
ui_behave_t ui_scene_poke(int index);           // plays the object's behaviour and returns it, so the caller can pick a sound
bool ui_scene_hit_gugu(int sx, int sy);
void ui_home_lamp(bool on);                     // the room's lamp (a touch on it toggles)
bool ui_home_lamp_is_on(void);
void ui_set_accessory(const char *id);          // "sun_hat" | "sunglasses" | "camera" | NULL: an overlay that follows the face

// ---- notice card (parent mode shows the hotspot name and password; nothing else puts text on screen) ----
void ui_show_notice(const char *title, const char *body);   // a full-screen card over the scene; body may have newlines
void ui_hide_notice(void);
