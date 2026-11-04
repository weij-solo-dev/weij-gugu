// ui_internal.h — private contract between ui.c (LVGL objects, pre-scaled images, the display) and
// ui_anim.c (timers that decide which sprite and which overlays are shown). Not for other modules.
#pragma once
#include "ui/ui.h"
#include "ui_sprites.h"
#include "lvgl.h"

#define GUGU_BASE_X (34 * UI_SPRITE_SCALE + 1)   // the pigeon's home position in screen pixels
#define GUGU_BASE_Y (40 * UI_SPRITE_SCALE + 1)   // 46 until 2026-11-04: lifted 6 rows with the floor (pixelgen ROOM_LIFT)

extern lv_obj_t *ui_gugu_img;                    // the pigeon: a pre-scaled base sprite; overlays are its children

void ui_anim_init(lv_obj_t *scr);                // creates the ring and timers, shows the first sprite
void ui_anim_emotion_changed(ui_emotion_t e);

// Services ui.c offers the animation layer. Call with the UI lock held or from an LVGL timer.
void ui_show_base(const lv_image_dsc_t *base);  // switch the pigeon's base sprite (scaled once into PSRAM)
void ui_show_beak(const ui_patch_t *p);         // beak-open overlay; NULL hides it
void ui_show_eyes(const ui_patch_t *p);         // eyes-closed overlay; NULL hides it

// ui_scene.c ↔ ui.c / ui_anim.c
extern bool ui_scene_is_home;                   // which hit table applies; ui.c sets it with the scene
void ui_scene_init(lv_obj_t *scr);              // the window pane and the object images, between the backdrop and the pigeon
void ui_scene_init_accessory(void);             // the accessory overlay, a child of the pigeon (after ui_gugu_img exists)
void ui_scene_show_window(bool show);
void ui_scene_refresh_accessory(ui_emotion_t e);   // reposition the worn accessory for the face on screen
void ui_home_backdrop_refresh(void);            // lamp on/off: swap the room picture (UI lock held)
